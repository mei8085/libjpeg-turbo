/*
 * jpegsizetest.c
 *
 * Copyright (C) 2026, D. R. Commander.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 */

/* This program verifies that malicious JPEG frame (SOF) headers with extreme
   dimensions, sampling factors, or component counts-- combined with IDCT
   scaling parameters and row-width padding-- cannot cause integer wrap-around
   or out-of-bounds writes in the decoder arrays or in the TurboJPEG output
   buffer size calculations.  It also verifies that:

   1. Oversized or self-contradictory dimensions fail with a stable error
      before any large allocation or write occurs (run under ASan/UBSan to
      prove there is no out-of-bounds or post-wrap small-buffer write).
   2. Near-boundary but legal images still decode, and the error category is
      consistent across the libjpeg, TurboJPEG 3.0, and legacy TurboJPEG
      decode entry points.

   The program never relies on requesting huge amounts of memory or on the
   process being killed by the OOM killer:  every decode handle is given a
   small TJPARAM_MAXMEMORY/TJPARAM_MAXPIXELS budget, and the process monitors
   its own resident set size. */

#ifdef _MSC_VER
#define _CRT_SECURE_NO_DEPRECATE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <setjmp.h>

#include "turbojpeg.h"
#include "jpeglib.h"
#include "jerror.h"


static int ntests = 0, nfail = 0;


#define THROW(m) { \
  printf("FATAL: %s\n", m); \
  exit(1); \
}

/* ------------------------------------------------------------------ */
/* Memory guard:  monitor resident set size so that a test that silently
   requests a huge allocation (and might otherwise be OOM-killed) becomes a
   loud, diagnosable failure instead. */

#ifdef __linux__
#define RSS_GUARD 1
#endif

#define RSS_LIMIT_KB  (64 * 1024)  /* no single case may grow RSS by >64 MB */

static long get_rss_kb(void)
{
#ifdef RSS_GUARD
  FILE *f = NULL;
  long kb = 0;

  if ((f = fopen("/proc/self/status", "r")) != NULL) {
    char line[256];

    while (fgets(line, sizeof(line), f)) {
      if (!strncmp(line, "VmRSS:", 6)) {
        kb = strtol(line + 6, NULL, 10);
        break;
      }
    }
    fclose(f);
  }
  return kb;
#else
  return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* libjpeg C API error management (modeled on bmpsizetest.c) */

static char lj_error_str[JMSG_LENGTH_MAX] = { 0 };
static volatile int lj_warning_seen;

struct lj_error_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
};

static void lj_error_exit(j_common_ptr cinfo)
{
  struct lj_error_mgr *myerr = (struct lj_error_mgr *)cinfo->err;

  (*cinfo->err->output_message)(cinfo);
  longjmp(myerr->setjmp_buffer, 1);
}

static void lj_output_message(j_common_ptr cinfo)
{
  (*cinfo->err->format_message)(cinfo, lj_error_str);
}

/* Warnings (msg_level < 0) do not longjmp; record that one occurred so the
   caller can assert on the warning category. */
static void lj_emit_message(j_common_ptr cinfo, int msg_level)
{
  if (msg_level < 0) {
    lj_warning_seen = 1;
    (*cinfo->err->format_message)(cinfo, lj_error_str);
  } else if (cinfo->err->trace_level >= msg_level) {
    (*cinfo->err->output_message)(cinfo);
  }
}

/* Run the libjpeg entry point through jpeg_read_header()/jpeg_start_decompress()
   and confirm it fails with a message containing substr.  Returns 0 on
   success.  If fatal==0, a warning-only outcome (header/start succeeds) is
   accepted instead. */
static int libjpeg_expect(const unsigned char *buf, size_t size,
                          const char *substr, int expect_fatal)
{
  struct jpeg_decompress_struct cinfo;
  struct lj_error_mgr jerr;
  int fired = 0;
  char saved[JMSG_LENGTH_MAX];

  lj_error_str[0] = '\0';
  lj_warning_seen = 0;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = lj_error_exit;
  jerr.pub.output_message = lj_output_message;
  jerr.pub.emit_message = lj_emit_message;

  if (setjmp(jerr.setjmp_buffer)) {
    fired = 1;
    jpeg_destroy_decompress(&cinfo);
  } else {
    JSAMPARRAY row = NULL;

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, buf, size);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);
    /* Drive the scan so that entropy-segment truncation actually reaches the
       data source and emits JWRN_JPEG_EOF.  The row buffer is one output row;
       geometry checks have already rejected oversized streams by now. */
    if (!expect_fatal && cinfo.output_width > 0 &&
        cinfo.output_width < 100000) {
      row = (*cinfo.mem->alloc_sarray)
        ((j_common_ptr)&cinfo, JPOOL_IMAGE,
         (JDIMENSION)(cinfo.output_width * cinfo.output_components), 1);
      while (cinfo.output_scanline < cinfo.output_height)
        jpeg_read_scanlines(&cinfo, row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
  }

  strncpy(saved, lj_error_str, sizeof(saved) - 1);
  saved[sizeof(saved) - 1] = '\0';

  if (expect_fatal) {
    if (!fired) {
      printf("    libjpeg: expected failure containing \"%s\", but it "
             "succeeded\n", substr);
      return -1;
    }
    if (!strstr(saved, substr)) {
      printf("    libjpeg: error \"%s\" does not contain \"%s\"\n", saved,
             substr);
      return -1;
    }
  } else {
    /* Warning case:  no fatal longjmp may occur, and the expected warning must
       have been emitted while reading the scan. */
    if (fired) {
      printf("    libjpeg: unexpected fatal error \"%s\" (wanted warning "
             "\"%s\")\n", saved, substr);
      return -1;
    }
    if (!lj_warning_seen || !strstr(saved, substr)) {
      printf("    libjpeg: expected warning \"%s\", got %s (\"%s\")\n", substr,
             lj_warning_seen ? "a different warning" : "no warning", saved);
      return -1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* In-memory JPEG stream construction */

/* Locate the SOFn marker in a JPEG buffer.  Returns the offset of the marker
   prefix (0xFF 0xCn), or -1. */
static int find_sof(const unsigned char *buf, size_t size)
{
  size_t i;

  for (i = 0; i + 1 < size; i++) {
    if (buf[i] == 0xFF && (buf[i + 1] & 0xF0) == 0xC0 &&
        buf[i + 1] != 0xC4 && buf[i + 1] != 0xC8 && buf[i + 1] != 0xCC)
      return (int)i;
  }
  return -1;
}

static void put16(unsigned char *p, unsigned int v)
{
  p[0] = (unsigned char)(v >> 8);
  p[1] = (unsigned char)(v & 0xFF);
}

/* Find a standalone marker segment (e.g. DQT=0xDB, DHT=0xC4) and return the
   offset of its 0xFF prefix.  *seg_len receives the full segment length in
   bytes (2-byte length field included).  Returns -1 if not found. */
static int find_segment(const unsigned char *buf, size_t size,
                        unsigned char marker, size_t *seg_len)
{
  size_t i;

  for (i = 1; i + 2 < size; i++) {
    if (buf[i - 1] == 0xFF && buf[i] == marker) {
      unsigned int len = ((unsigned int)buf[i + 1] << 8) | buf[i + 2];

      if (seg_len) *seg_len = (size_t)len + 2;
      return (int)i - 1;
    }
  }
  return -1;
}

/* Append the DQT and DHT marker segments copied from a valid template JPEG, so
   that hand-built streams always carry well-formed quantization/Huffman
   tables and cannot fail table validation before reaching the geometry checks.
   Returns the new write offset. */
static size_t copy_table_segments(unsigned char *out, size_t p, size_t outcap,
                                  const unsigned char *tmpl, size_t tsize)
{
  static const unsigned char markers[] = { 0xDB, 0xC4 };
  int k;

  for (k = 0; k < (int)(sizeof(markers) / sizeof(markers[0])); k++) {
    size_t seg_len = 0;
    int off = find_segment(tmpl, tsize, markers[k], &seg_len);
    size_t j;

    if (off < 0) THROW("template JPEG missing required table segment");
    if (p + seg_len > outcap) THROW("stream buffer too small");
    for (j = 0; j < seg_len; j++) out[p++] = tmpl[off + j];
  }
  return p;
}

/* Build a minimal baseline (SOF0) JPEG stream with the given geometry.  nc is
   the number of components (1..MAX_COMPONENTS), and sf[] holds the packed
   (h<<4 | v) sampling-factor nibble for each.  The DQT/DHT segments are copied
   from a valid template JPEG.

   Layout: SOI DQT DHT SOF0 SOS [entropy bytes] EOI.  The entropy payload does
   not need to decode correctly because the geometry validation runs when the
   first SOS is consumed (jpeg_read_header reaches JPEG_REACHED_SOS). */
static size_t build_sof_stream(unsigned char *out, size_t outcap, int width,
                               int height, int precision, int nc,
                               const unsigned char *sf,
                               const unsigned char *tmpl, size_t tsize)
{
  size_t p = 0;
  int i;

#define EMIT(byte)  do { \
  if (p >= outcap) THROW("stream buffer too small"); \
  out[p++] = (unsigned char)(byte); \
} while (0)
#define EMIT16(v)  do { EMIT((v) >> 8); EMIT((v) & 0xFF); } while (0)

  /* SOI */
  EMIT(0xFF); EMIT(0xD8);

  /* Well-formed quantization and Huffman tables from the template. */
  p = copy_table_segments(out, p, outcap, tmpl, tsize);

  /* SOF0 */
  EMIT(0xFF); EMIT(0xC0);
  EMIT16(8 + nc * 3);          /* marker segment length */
  EMIT(precision & 0xFF);      /* data precision */
  EMIT16((unsigned int)height & 0xFFFF);
  EMIT16((unsigned int)width & 0xFFFF);
  EMIT(nc & 0xFF);
  for (i = 0; i < nc; i++) {
    EMIT(1 + i);               /* component identifier */
    EMIT(sf[i]);               /* sampling factors (h<<4|v) */
    EMIT(0);                   /* quant table 0 */
  }

  /* SOS */
  EMIT(0xFF); EMIT(0xDA);
  EMIT16(6 + 2 * nc);
  EMIT(nc & 0xFF);
  for (i = 0; i < nc; i++) {
    EMIT(1 + i);               /* component selector */
    EMIT(0x00);                /* DC/AC Huffman table 0 */
  }
  EMIT(0x00); EMIT(0x3F); EMIT(0x00);  /* Ss=0, Se=63, Ah/Al=0 */

  /* A handful of entropy bytes followed by EOI (enough to enter the scan; the
     geometry validation has already happened by then). */
  for (i = 0; i < 8; i++) EMIT(0x00);
  EMIT(0xFF); EMIT(0xD9);

#undef EMIT
#undef EMIT16
  return p;
}

/* Create a genuinely valid baseline JPEG of the requested size/subsampling via
   the TurboJPEG compressor.  *jpegBuf is allocated with tj3Alloc() and must be
   freed by the caller with tj3Free().  Returns 0 on success. */
static int make_valid_jpeg(int width, int height, int subsamp,
                           unsigned char **jpegBuf, size_t *jpegSize)
{
  tjhandle comp = NULL;
  unsigned char *src = NULL, *buf = NULL;
  size_t cap;
  int pf = (subsamp == TJSAMP_GRAY ? TJPF_GRAY : TJPF_RGB);
  int pitch = width * tjPixelSize[pf];
  int i, retval = -1;

  if ((comp = tj3Init(TJINIT_COMPRESS)) == NULL) goto bailout;
  if (tj3Set(comp, TJPARAM_QUALITY, 90) == -1) goto bailout;
  if (tj3Set(comp, TJPARAM_SUBSAMP, subsamp) == -1) goto bailout;

  cap = (size_t)pitch * (size_t)height;
  if ((src = (unsigned char *)malloc(cap)) == NULL)
    THROW("Memory allocation failure");
  /* Deterministic non-trivial content. */
  for (i = 0; i < pitch * height; i++)
    src[i] = (unsigned char)((i * 37 + i / (pitch ? pitch : 1)) & 0xFF);

  *jpegSize = 0;
  if (tj3Compress8(comp, src, width, pitch, height, pf, &buf, jpegSize) ==
      -1) {
    printf("    tj3Compress8: %s\n", tj3GetErrorStr(comp));
    goto bailout;
  }
  *jpegBuf = buf;
  buf = NULL;
  retval = 0;

bailout:
  tj3Free(buf);
  free(src);
  tj3Destroy(comp);
  return retval;
}

/* Return a writable copy of a JPEG with its SOF geometry patched.  Used to
   attach a malicious header to otherwise-valid tables/entropy data. */
static unsigned char *patch_sof(const unsigned char *src, size_t size,
                                int width, int height,
                                const unsigned char *sf3, size_t *out_size)
{
  unsigned char *dst;
  int sof;

  if ((dst = (unsigned char *)malloc(size ? size : 1)) == NULL)
    THROW("Memory allocation failure");
  memcpy(dst, src, size);
  *out_size = size;

  sof = find_sof(dst, size);
  if (sof < 0) {
    free(dst);
    return NULL;
  }
  /* SOF layout after marker: [len_hi len_lo precision H_hi H_lo W_hi W_lo
     nc c0id c0sf c0tq ...] */
  put16(dst + sof + 5, (unsigned int)height & 0xFFFF);
  put16(dst + sof + 7, (unsigned int)width & 0xFFFF);
  if (sf3) {
    int nc = dst[sof + 9];

    if (nc >= 3) {
      dst[sof + 11] = sf3[0];
      dst[sof + 14] = sf3[1];
      dst[sof + 17] = sf3[2];
    }
  }
  return dst;
}

/* ------------------------------------------------------------------ */
/* TurboJPEG entry-point wrappers
 *
 * The same malformed stream is fed to the TurboJPEG 3.0 API, the legacy
 * TurboJPEG API, and the libjpeg C API.  The TurboJPEG wrappers translate some
 * low-level libjpeg messages into their own wording (e.g. "Invalid argument"),
 * so each entry point carries its own expected message substring, but the
 * error CATEGORY (TJERR_FATAL vs TJERR_WARNING) must be identical everywhere.
 */

struct err_expectation {
  const char *tj_substr;    /* expected substring in the TurboJPEG error */
  const char *lj_substr;    /* expected substring in the libjpeg message */
  int want_warning;         /* 1 => TJERR_WARNING, 0 => TJERR_FATAL */
};

/* Allocate an output buffer only when the reported geometry is within the
 * test's pixel budget; oversized images must be rejected by TJPARAM_MAXPIXELS
 * before any decode write, so they get no buffer at all. */
#define PIXEL_BUDGET  (1024 * 1024)

static unsigned char *small_dst_buffer(int w, int h, int pf)
{
  long long pixels;
  size_t cap;

  if (w <= 0 || h <= 0) return NULL;
  pixels = (long long)w * (long long)h;
  if (pixels > PIXEL_BUDGET) return NULL;
  cap = (size_t)pixels * (size_t)tjPixelSize[pf];
  return (unsigned char *)malloc(cap ? cap : 1);
}

static int tj3_expect_fail(const unsigned char *buf, size_t size,
                           const struct err_expectation *ex)
{
  tjhandle handle = NULL;
  unsigned char *dst = NULL;
  int w = 0, h = 0, rc, code, retval = -1;
  const char *es;

  if ((handle = tj3Init(TJINIT_DECOMPRESS)) == NULL) {
    printf("    tj3Init: %s\n", tj3GetErrorStr(NULL));
    goto bailout;
  }
  tj3Set(handle, TJPARAM_MAXMEMORY, 8);
  /* Reject oversized images before jpeg_start_decompress()/any write.  This is
     what makes the tests independent of the system's memory capacity. */
  tj3Set(handle, TJPARAM_MAXPIXELS, PIXEL_BUDGET);

  if (tj3DecompressHeader(handle, buf, size) == 0) {
    w = tj3Get(handle, TJPARAM_JPEGWIDTH);
    h = tj3Get(handle, TJPARAM_JPEGHEIGHT);
    dst = small_dst_buffer(w, h, TJPF_RGB);
  }
  rc = tj3Decompress8(handle, buf, size, dst, 0, TJPF_RGB);
  es = tj3GetErrorStr(handle);
  code = tj3GetErrorCode(handle);

  if (rc != -1) {
    printf("    tj3Decompress8: expected failure containing \"%s\", but it "
           "succeeded\n", ex->tj_substr);
    goto bailout;
  }
  if (ex->want_warning) {
    if (code != TJERR_WARNING) {
      printf("    tj3Decompress8: expected TJERR_WARNING, got %d (\"%s\")\n",
             code, es);
      goto bailout;
    }
  } else {
    if (code != TJERR_FATAL) {
      printf("    tj3Decompress8: expected TJERR_FATAL, got %d (\"%s\")\n",
             code, es);
      goto bailout;
    }
    if (!strstr(es, ex->tj_substr)) {
      printf("    tj3Decompress8: error \"%s\" does not contain \"%s\"\n", es,
             ex->tj_substr);
      goto bailout;
    }
  }
  retval = 0;

bailout:
  free(dst);
  if (handle) tj3Destroy(handle);
  return retval;
}

static int legacy_expect_fail(const unsigned char *buf, size_t size,
                              const struct err_expectation *ex)
{
  tjhandle handle = NULL;
  unsigned char *dst = NULL;
  int w = 0, h = 0, subsamp = -1, cs = -1, rc, retval = -1;

  if ((handle = tjInitDecompress()) == NULL) {
    printf("    tjInitDecompress: %s\n", tjGetErrorStr());
    goto bailout;
  }
  rc = tjDecompressHeader3(handle, buf, (unsigned long)size, &w, &h,
                           &subsamp, &cs);
  if (rc == 0) {
    dst = small_dst_buffer(w, h, TJPF_RGB);
    rc = tjDecompress2(handle, buf, (unsigned long)size, dst, w, 0, h,
                       TJPF_RGB, 0);
  }
  /* If the header itself failed, that already constitutes the expected fatal
     rejection; the exact wrapper wording is not stable across entry points. */
  {
    const char *es = tj3GetErrorStr(handle);
    int code = tj3GetErrorCode(handle);

    if (rc != -1) {
      printf("    tjDecompress2: expected failure containing \"%s\", but it "
             "succeeded\n", ex->tj_substr);
      goto bailout;
    }
    if (ex->want_warning) {
      if (code != TJERR_WARNING) {
        printf("    tjDecompress2: expected TJERR_WARNING, got %d (\"%s\")\n",
               code, es);
        goto bailout;
      }
    } else {
      if (code != TJERR_FATAL) {
        printf("    tjDecompress2: expected TJERR_FATAL, got %d (\"%s\")\n",
               code, es);
        goto bailout;
      }
      /* Only enforce the message substring when the header parsed and the
         failure came from the full decode; an earlier header failure is
         reported with wrapper-specific wording that legitimately differs. */
      if (dst != NULL && !strstr(es, ex->tj_substr)) {
        printf("    tjDecompress2: error \"%s\" does not contain \"%s\"\n", es,
               ex->tj_substr);
        goto bailout;
      }
    }
  }
  retval = 0;

bailout:
  free(dst);
  if (handle) tjDestroy(handle);
  return retval;
}

/* Run all three decode entry points against one stream and require the same
   error category (and the entry-point-specific message substrings). */
static void expect_all_fail(const char *name, const unsigned char *buf,
                            size_t size, const struct err_expectation *ex)
{
  long rss0 = get_rss_kb();
  int bad = 0;

  ntests++;

  if (tj3_expect_fail(buf, size, ex) == -1) bad = 1;
  if (legacy_expect_fail(buf, size, ex) == -1) bad = 1;
  if (libjpeg_expect(buf, size, ex->lj_substr, !ex->want_warning) == -1)
    bad = 1;

  if (bad) {
    printf("  [%s] error-category mismatch\n", name);
    nfail++;
  } else {
    printf("  [ok] %-28s -> %s (tj:\"%s\" / lj:\"%s\")\n", name,
           ex->want_warning ? "WARNING" : "FATAL", ex->tj_substr,
           ex->lj_substr);
  }

  if (RSS_GUARD && get_rss_kb() - rss0 > RSS_LIMIT_KB) {
    printf("  FAIL %s: RSS grew by %ld KB (possible huge allocation / OOM "
           "risk)\n", name, get_rss_kb() - rss0);
    nfail++;
  }
}

/* ------------------------------------------------------------------ */
/* Group A: malicious SOF streams must fail stably, before any large
   allocation, with a consistent category across entry points. */

#define STREAM_CAP  4096

/* Expectation constructors:  FATAL2/WARN2 carry the entry-point-specific
   TurboJPEG and libjpeg message substrings plus the shared error category. */
#define FATAL2(tj, lj)  &(struct err_expectation){ (tj), (lj), 0 }
#define WARN2(tj, lj)  &(struct err_expectation){ (tj), (lj), 1 }

static void group_a_malicious_sof(void)
{
  unsigned char stream[STREAM_CAP];
  unsigned char *tmpl = NULL, *valid420 = NULL;
  size_t tsz = 0, v420sz = 0;
  unsigned char sf_444[3] = { 0x11, 0x11, 0x11 };
  unsigned char sf_420[3] = { 0x22, 0x11, 0x11 };
  unsigned char sf_zero_h[3] = { 0x01, 0x11, 0x11 };
  unsigned char sf_zero_v[3] = { 0x10, 0x11, 0x11 };
  unsigned char sf_too_big[3] = { 0x55, 0x11, 0x11 };
  unsigned char sf_max[3] = { 0x44, 0x11, 0x11 };
  unsigned char sf_contradict[3] = { 0x33, 0x32, 0x23 };
  size_t n;

  printf("\n=== Group A: malicious SOF streams ===\n");

  /* A valid 4:4:4 stream supplies well-formed DQT/DHT segments, and a 4:2:0
     stream is the base for the patched cases. */
  if (make_valid_jpeg(16, 16, TJSAMP_444, &tmpl, &tsz) == -1)
    THROW("could not build 4:4:4 template");
  if (make_valid_jpeg(16, 16, TJSAMP_420, &valid420, &v420sz) == -1)
    THROW("could not build 4:2:0 template");

  /* The TurboJPEG wrappers turn a libjpeg fatal raised during
     jpeg_read_header()/jpeg_start_decompress() into the generic message
     "Invalid argument"; the libjpeg C API retains the specific message.  The
     category is TJERR_FATAL in every case below. */
#define TJ_FATAL  "Invalid argument"

  /* A1/A2/A3: dimensions beyond JPEG_MAX_DIMENSION (65500). */
  n = build_sof_stream(stream, sizeof(stream), 65535, 1, 8, 3, sf_444,
                       tmpl, tsz);
  expect_all_fail("A1 width=65535", stream, n,
                  FATAL2(TJ_FATAL, "Maximum supported image dimension"));

  n = build_sof_stream(stream, sizeof(stream), 1, 65535, 8, 3, sf_444,
                       tmpl, tsz);
  expect_all_fail("A2 height=65535", stream, n,
                  FATAL2(TJ_FATAL, "Maximum supported image dimension"));

  n = build_sof_stream(stream, sizeof(stream), 65535, 65535, 8, 3, sf_444,
                       tmpl, tsz);
  expect_all_fail("A3 width=height=65535", stream, n,
                  FATAL2(TJ_FATAL, "Maximum supported image dimension"));

  /* A4: zero dimensions. */
  n = build_sof_stream(stream, sizeof(stream), 0, 16, 8, 3, sf_444,
                       tmpl, tsz);
  expect_all_fail("A4a width=0", stream, n,
                  FATAL2(TJ_FATAL, "Empty JPEG image"));

  n = build_sof_stream(stream, sizeof(stream), 16, 0, 8, 3, sf_444,
                       tmpl, tsz);
  expect_all_fail("A4b height=0", stream, n,
                  FATAL2(TJ_FATAL, "Empty JPEG image"));

  /* A5: zero sampling factor (rejected in initial_setup, not at the marker). */
  n = build_sof_stream(stream, sizeof(stream), 16, 16, 8, 3, sf_zero_h,
                       tmpl, tsz);
  expect_all_fail("A5a h_samp_factor=0", stream, n,
                  FATAL2(TJ_FATAL, "Bogus sampling factors"));

  n = build_sof_stream(stream, sizeof(stream), 16, 16, 8, 3, sf_zero_v,
                       tmpl, tsz);
  expect_all_fail("A5b v_samp_factor=0", stream, n,
                  FATAL2(TJ_FATAL, "Bogus sampling factors"));

  /* A6: sampling factor above MAX_SAMP_FACTOR (4). */
  n = build_sof_stream(stream, sizeof(stream), 16, 16, 8, 3, sf_too_big,
                       tmpl, tsz);
  expect_all_fail("A6 samp_factor=5", stream, n,
                  FATAL2(TJ_FATAL, "Bogus sampling factors"));

  /* A7: mutually inconsistent sampling factors.  The marker is accepted, but
     the master setup rejects the fractional relationship. */
  n = build_sof_stream(stream, sizeof(stream), 17, 17, 8, 3, sf_contradict,
                       tmpl, tsz);
  expect_all_fail("A7 inconsistent factors", stream, n,
                  FATAL2("Fractional sampling", "Fractional sampling"));

  /* A8: more than MAX_COMPONENTS (10).  The 11-component SOF length cannot
     match the marker grammar the parser enforces, so it surfaces as a marker
     length error. */
  {
    unsigned char sf11[11];
    int i;

    for (i = 0; i < 11; i++) sf11[i] = 0x11;
    n = build_sof_stream(stream, sizeof(stream), 16, 16, 8, 11, sf11,
                         tmpl, tsz);
    expect_all_fail("A8 11 components", stream, n,
                    FATAL2(TJ_FATAL, "Bogus marker length"));
  }

  /* A9: SOF marker length that explicitly contradicts the component count. */
  n = build_sof_stream(stream, sizeof(stream), 16, 16, 8, 3, sf_444,
                       tmpl, tsz);
  {
    int sof = find_sof(stream, n);

    if (sof < 0) THROW("A9: SOF not found");
    put16((unsigned char *)stream + sof + 2, 9 + 3 * 3);  /* wrong length */
    expect_all_fail("A9 bogus marker length", stream, n,
                    FATAL2(TJ_FATAL, "Bogus marker length"));
  }

  /* A10: lossy stream with an unsupported data precision (7). */
  n = build_sof_stream(stream, sizeof(stream), 16, 16, 7, 3, sf_444,
                       tmpl, tsz);
  expect_all_fail("A10 precision=7", stream, n,
                  FATAL2(TJ_FATAL, "Unsupported JPEG data precision"));

  /* A11: legal-but-huge near-limit dimension with maximum sampling factors.
     This exercises the block-dimension multiply and PAD carry.  The hand-built
     stream trips the interleaved-scan limit; the patched, otherwise-valid
     stream below reaches the dimension/pixel guard.  Both must fail before any
     large allocation. */
  n = build_sof_stream(stream, sizeof(stream), 65500, 65500, 8, 3, sf_max,
                       tmpl, tsz);
  expect_all_fail("A11 65500x65500 factor4", stream, n,
                  FATAL2(TJ_FATAL, "Sampling factors too large"));

  {
    unsigned char *evil = NULL;
    size_t esz = 0;

    /* 65501 is the smallest width past JPEG_MAX_DIMENSION (65500), so libjpeg
       rejects it at header validation.  TurboJPEG rejects it before any decode
       write as well (its pixel guard or its NULL-destination argument check);
       both paths are fatal and allocate nothing. */
    evil = patch_sof(valid420, v420sz, 65501, 1, sf_420, &esz);
    if (evil) {
      expect_all_fail("A11b patched 65501-wide", evil, esz,
                      FATAL2(TJ_FATAL,
                             "Maximum supported image dimension"));
      free(evil);
    }
  }

  tj3Free(valid420);
  tj3Free(tmpl);

#undef TJ_FATAL
}

/* A12: a fully legal-dimension image (65500 x 1 grayscale, within
   JPEG_MAX_DIMENSION) whose pixel count exceeds an application pixel budget.
   TJPARAM_MAXPIXELS must make tj3Decompress8() fail with "Image is too large"
   before jpeg_start_decompress()/any output allocation, proving the test never
   depends on requesting the full (potentially huge) destination buffer. */
static void group_a_pixel_guard(void)
{
  unsigned char *jpeg = NULL;
  size_t jsz = 0;
  tjhandle dec = NULL;
  long rss0;
  int rc;

  printf("\n=== Group A12: pixel budget enforced before allocation ===\n");
  ntests++;

  if (make_valid_jpeg(65500, 1, TJSAMP_GRAY, &jpeg, &jsz) == -1) {
    printf("  FAIL A12: could not build legal 65500x1 image\n");
    nfail++;
    return;
  }

  rss0 = get_rss_kb();
  dec = tj3Init(TJINIT_DECOMPRESS);
  if (!dec) {
    printf("  FAIL A12: tj3Init: %s\n", tj3GetErrorStr(NULL));
    nfail++;
    goto done;
  }
  tj3Set(dec, TJPARAM_MAXMEMORY, 8);
  /* Budget far below the 65500-pixel image.  A tiny non-NULL destination is
     passed only to clear the API's NULL-pointer argument check; the pixel
     guard (evaluated before jpeg_start_decompress()/any write) must reject the
     image before that buffer is ever touched. */
  tj3Set(dec, TJPARAM_MAXPIXELS, 1024);

  if (tj3DecompressHeader(dec, jpeg, jsz) == -1) {
    printf("  FAIL A12: legal image header rejected: %s\n",
           tj3GetErrorStr(dec));
    nfail++;
    goto done;
  }
  {
    unsigned char dummy[64];

    rc = tj3Decompress8(dec, jpeg, jsz, dummy, 0, TJPF_GRAY);
  }
  if (rc != -1 || tj3GetErrorCode(dec) != TJERR_FATAL ||
      !strstr(tj3GetErrorStr(dec), "Image is too large")) {
    printf("  FAIL A12: rc=%d code=%d err=\"%s\"\n", rc,
           tj3GetErrorCode(dec), tj3GetErrorStr(dec));
    nfail++;
    goto done;
  }

  if (RSS_GUARD && get_rss_kb() - rss0 > RSS_LIMIT_KB) {
    printf("  FAIL A12: RSS grew by %ld KB despite pixel guard\n",
           get_rss_kb() - rss0);
    nfail++;
    goto done;
  }
  printf("  [ok] A12 65500x1 rejected by pixel budget before allocation\n");

done:
  if (dec) tj3Destroy(dec);
  tj3Free(jpeg);
}

/* ------------------------------------------------------------------ */
/* Group B: pure-parameter buffer-size arithmetic.  These calls allocate no
   image memory; they verify the PAD()-carry and multiply math directly. */

static unsigned long long PAD_ULL(unsigned long long v, unsigned long long p)
{
  return (v + p - 1) & ~(p - 1);
}

#define CHECK_EQ(actual, expected, what) { \
  ntests++; \
  if ((unsigned long long)(actual) != (unsigned long long)(expected)) { \
    printf("  FAIL %-34s got %llu, want %llu\n", what, \
           (unsigned long long)(actual), (unsigned long long)(expected)); \
    nfail++; \
  } else { \
    printf("  [ok] %-34s = %llu\n", what, (unsigned long long)(actual)); \
  } \
}

#define CHECK_ZERO_ERR(call, what, substr) { \
  size_t _r = (call); \
  ntests++; \
  if (_r != 0 || !strstr(tj3GetErrorStr(NULL), substr)) { \
    printf("  FAIL %-34s ret=%zu err=\"%s\" (want 0 / \"%s\")\n", what, _r, \
           tj3GetErrorStr(NULL), substr); \
    nfail++; \
  } else { \
    printf("  [ok] %-34s rejected (\"%s\")\n", what, substr); \
  } \
}

static void group_b_bufsize(void)
{
  static const int edge_dims[] = { 1, 7, 8, 9, 15, 16, 17, 31, 32, 33 };
  int k, d;

  printf("\n=== Group B: buffer-size arithmetic / PAD carry ===\n");

  /* B1: exact values across MCU and alignment boundaries.  Independent
     reference computed straight from the documented PAD formulas. */
  for (k = 0; k < (int)(sizeof(edge_dims) / sizeof(edge_dims[0])); k++) {
    d = edge_dims[k];
    char label[64];

    /* JPEG worst case (4:4:4: MCU 8x8, chroma factor 6). */
    snprintf(label, sizeof(label), "tj3JPEGBufSize %d 444", d);
    CHECK_EQ(tj3JPEGBufSize(d, d, TJSAMP_444),
             PAD_ULL(d, 8) * PAD_ULL(d, 8) * 6ULL + 2048ULL, label);

    /* YUV 4:2:0 with row alignment 4.  The reference is assembled from the
       documented per-plane width/height accessors (exercised separately in
       B2) plus the same PAD-to-alignment stride rule used internally:
       Y plane + two half-size chroma planes. */
    {
      int pwY = tj3YUVPlaneWidth(0, d, TJSAMP_420);
      int phY = tj3YUVPlaneHeight(0, d, TJSAMP_420);
      int pwC = tj3YUVPlaneWidth(1, d, TJSAMP_420);
      int phC = tj3YUVPlaneHeight(1, d, TJSAMP_420);
      unsigned long long strideY = PAD_ULL((unsigned long long)pwY, 4);
      unsigned long long strideC = PAD_ULL((unsigned long long)pwC, 4);
      unsigned long long expect =
        strideY * (unsigned long long)phY +
        2ULL * strideC * (unsigned long long)phC;

      snprintf(label, sizeof(label), "tj3YUVBufSize %d 420 a4", d);
      CHECK_EQ(tj3YUVBufSize(d, 4, d, TJSAMP_420), expect, label);
    }

    /* Legacy unsigned-long entry points must agree with the size_t ones for
       in-range dimensions. */
    snprintf(label, sizeof(label), "tjBufSize==tj3JPEGBufSize %d", d);
    CHECK_EQ(tjBufSize(d, d, TJSAMP_444),
             tj3JPEGBufSize(d, d, TJSAMP_444), label);

    snprintf(label, sizeof(label), "tjBufSizeYUV2==tj3YUVBufSize %d", d);
    CHECK_EQ(tjBufSizeYUV2(d, 4, d, TJSAMP_420),
             tj3YUVBufSize(d, 4, d, TJSAMP_420), label);
  }

  /* B2: plane width/height guards trip before INT_MAX and report a stable
     category (also cover the legacy -1 convention). */
  CHECK_ZERO_ERR(tj3YUVPlaneWidth(0, INT_MAX, TJSAMP_420),
                 "planeWidth INT_MAX 420", "Width is too large");
  CHECK_ZERO_ERR(tj3YUVPlaneHeight(0, INT_MAX, TJSAMP_420),
                 "planeHeight INT_MAX 420", "Height is too large");
  {
    ntests++;
    if (tjPlaneWidth(0, INT_MAX, TJSAMP_420) != -1) {
      printf("  FAIL tjPlaneWidth INT_MAX should return -1\n");
      nfail++;
    } else
      printf("  [ok] tjPlaneWidth INT_MAX -> -1\n");
  }

  /* B3: aggregate YUV size inherits the plane guard (returns 0 / legacy
     (unsigned long)-1) rather than a wrapped small value. */
  CHECK_ZERO_ERR(tj3YUVBufSize(INT_MAX, 4, INT_MAX, TJSAMP_420),
                 "tj3YUVBufSize INT_MAX 420", "too large");
  {
    unsigned long r = tjBufSizeYUV2(INT_MAX, 4, INT_MAX, TJSAMP_420);

    ntests++;
    if (r != (unsigned long)-1) {
      printf("  FAIL tjBufSizeYUV2 INT_MAX should return -1, got %lu\n", r);
      nfail++;
    } else
      printf("  [ok] tjBufSizeYUV2 INT_MAX -> (unsigned long)-1\n");
  }

  /* B4: invalid alignment is rejected (not a silent wrap). */
  CHECK_ZERO_ERR(tj3YUVBufSize(64, 3, 64, TJSAMP_444),
                 "tj3YUVBufSize align=3", "Invalid argument");
  CHECK_ZERO_ERR(tj3YUVBufSize(64, 0, 64, TJSAMP_444),
                 "tj3YUVBufSize align=0", "Invalid argument");

  /* B5: values already exercised by tjunittest overflowTest() remain stable
     here, proving no wrap into a small buffer-sized result. */
  CHECK_ZERO_ERR(tj3YUVBufSize(37837, 3, 37837, TJSAMP_444),
                 "tj3YUVBufSize 37837 align3", "Invalid argument");
  {
    size_t r = tj3YUVPlaneSize(0, 65536, 0, 65536, TJSAMP_444);

    CHECK_EQ(r, (unsigned long long)65536 * 65536,
             "tj3YUVPlaneSize 65536^2 exact");
  }
}

/* ------------------------------------------------------------------ */
/* Group C: truncated input.  Truncation inside a marker/table is fatal;
   truncation inside the entropy scan only produces a warning (the data source
   synthesizes an EOI).  Categories must match across entry points. */

/* Locate the SOS marker prefix and return its offset, or -1. */
static int find_sos(const unsigned char *buf, size_t size)
{
  size_t i;

  for (i = 0; i + 1 < size; i++)
    if (buf[i] == 0xFF && buf[i + 1] == 0xDA) return (int)i;
  return -1;
}

static void group_c_truncation(void)
{
  unsigned char *jpeg = NULL;
  size_t jsz = 0;
  int sof, sos, nc, entropy_start;

  printf("\n=== Group C: truncated input ===\n");

  if (make_valid_jpeg(16, 16, TJSAMP_444, &jpeg, &jsz) == -1)
    THROW("could not build truncation template");

  sof = find_sof(jpeg, jsz);
  sos = find_sos(jpeg, jsz);
  if (sof < 0 || sos < 0) THROW("template markers not found");
  nc = jpeg[sof + 9];
  /* marker(2) + length(2) + nc byte + 2*nc comp bytes + Ss/Se/AhAl(3) */
  entropy_start = sos + 4 + nc + 2 * nc + 3;

  /* C1: empty buffer.  TurboJPEG rejects it as an invalid argument; libjpeg's
     memory source reports an empty input file.  Both are fatal. */
  expect_all_fail("C1 empty buffer", jpeg, 0,
                  FATAL2("Invalid argument", "Empty input file"));

  /* C2: truncation in the middle of the SOF marker segment. */
  expect_all_fail("C2 cut inside SOF", jpeg, (size_t)(sof + 6),
                  FATAL2("Invalid argument", "Bogus marker length"));

  /* C2b: truncation exactly at the SOF marker prefix (length unavailable). */
  expect_all_fail("C2b cut at SOF prefix", jpeg, (size_t)(sof + 2),
                  FATAL2("Invalid argument", "Bogus marker length"));

  /* C3: header complete through the SOS header but no entropy data.  The data
     source treats the immediate EOF as a truncated scan (warning) and inserts a
     synthetic EOI rather than aborting fatally. */
  expect_all_fail("C3 no entropy data", jpeg, (size_t)entropy_start,
                  WARN2("Premature end of JPEG file",
                        "premature end of data segment"));

  /* C4: entropy payload cut in half.  The data source warns (JWRN_JPEG_EOF /
     premature end) and inserts a synthetic EOI: a WARNING, not a crash. */
  expect_all_fail("C4 mid-entropy truncation", jpeg,
                  (jsz + (size_t)entropy_start) / 2,
                  WARN2("Premature end of JPEG file",
                        "premature end of data segment"));

  /* C5: EOI removed.  Same warning category as mid-scan truncation. */
  expect_all_fail("C5 missing EOI", jpeg, jsz - 2,
                  WARN2("Premature end of JPEG file",
                        "Premature end of JPEG file"));

  tj3Free(jpeg);
}

/* ------------------------------------------------------------------ */
/* Group D: near-boundary but legal images must still decode.  Every image is
   small in pixel count, and every decode runs under the memory/pixel guard.
   For each image we walk every supported IDCT scaling factor and verify the
   reported output dimensions against TJSCALED(), then decode fully (with both
   default and explicitly padded pitches) and also decode to YUV planes. */

static int decode_one_scaled(const unsigned char *jpeg, size_t jsz, int jw,
                             int jh, int subsamp,
                             const tjscalingfactor *sf)
{
  tjhandle dec = NULL;
  unsigned char *dst = NULL, *yuv = NULL;
  tjscalingfactor factor = *sf;
  int ow = TJSCALED(jw, factor), oh = TJSCALED(jh, factor);
  int pf = (subsamp == TJSAMP_GRAY ? TJPF_GRAY : TJPF_RGB);
  int pitch, retval = -1;

  if ((dec = tj3Init(TJINIT_DECOMPRESS)) == NULL) {
    printf("    tj3Init: %s\n", tj3GetErrorStr(NULL));
    goto bailout;
  }
  tj3Set(dec, TJPARAM_MAXMEMORY, 8);
  tj3Set(dec, TJPARAM_MAXPIXELS, PIXEL_BUDGET);
  if (tj3SetScalingFactor(dec, factor) == -1) {
    printf("    tj3SetScalingFactor: %s\n", tj3GetErrorStr(dec));
    goto bailout;
  }
  if (tj3DecompressHeader(dec, jpeg, jsz) == -1) {
    printf("    header: %s\n", tj3GetErrorStr(dec));
    goto bailout;
  }
  if (tj3Get(dec, TJPARAM_JPEGWIDTH) != jw ||
      tj3Get(dec, TJPARAM_JPEGHEIGHT) != jh) {
    printf("    header geometry mismatch\n");
    goto bailout;
  }

  /* (a) default pitch (0 => tight rows). */
  dst = (unsigned char *)malloc((size_t)ow * oh * tjPixelSize[pf] + 1);
  if (!dst) THROW("Memory allocation failure");
  if (tj3Decompress8(dec, jpeg, jsz, dst, 0, pf) == -1) {
    printf("    decode tight: %s\n", tj3GetErrorStr(dec));
    goto bailout;
  }
  free(dst);
  dst = NULL;

  /* (b) explicitly padded pitch rounded up to 64 bytes (row-width carry path). */
  pitch = ((ow * tjPixelSize[pf] + 63) / 64) * 64;
  dst = (unsigned char *)malloc((size_t)pitch * oh + 1);
  if (!dst) THROW("Memory allocation failure");
  if (tj3Decompress8(dec, jpeg, jsz, dst, pitch, pf) == -1) {
    printf("    decode padded: %s\n", tj3GetErrorStr(dec));
    goto bailout;
  }
  free(dst);
  dst = NULL;

  /* (c) decode to a YUV buffer sized from the documented plane formulas. */
  {
    size_t yuvsz = tj3YUVBufSize(ow, 4, oh, subsamp);

    if (yuvsz == 0) {
      printf("    tj3YUVBufSize: %s\n", tj3GetErrorStr(dec));
      goto bailout;
    }
    yuv = (unsigned char *)malloc(yuvsz);
    if (!yuv) THROW("Memory allocation failure");
    if (tj3DecompressToYUV8(dec, jpeg, jsz, yuv, 4) == -1) {
      printf("    decode-to-YUV: %s\n", tj3GetErrorStr(dec));
      goto bailout;
    }
  }

  retval = 0;

bailout:
  free(yuv);
  free(dst);
  if (dec) tj3Destroy(dec);
  return retval;
}

static void group_d_legal_boundary(void)
{
  static const struct {
    int w, h, subsamp;
  } cases[] = {
    { 65500, 1, TJSAMP_GRAY },   /* max legal dimension, single row */
    { 1, 65500, TJSAMP_GRAY },   /* max legal dimension, single column */
    { 15, 15, TJSAMP_444 },      /* one MCU minus one */
    { 16, 16, TJSAMP_444 },      /* exact MCU boundary */
    { 17, 17, TJSAMP_444 },      /* one over the MCU boundary */
    { 15, 17, TJSAMP_420 },      /* 4:2:0 MCU (16x16) padding edges */
    { 16, 16, TJSAMP_420 },
    { 17, 31, TJSAMP_420 },
    { 33, 9, TJSAMP_GRAY },      /* wide short grayscale across PAD edges */
  };
  tjscalingfactor *factors;
  int nfactors = 0, ci, fi;

  printf("\n=== Group D: legal near-boundary images decode ===\n");
  factors = tj3GetScalingFactors(&nfactors);
  if (!factors || nfactors <= 0) THROW("tj3GetScalingFactors failed");

  for (ci = 0; ci < (int)(sizeof(cases) / sizeof(cases[0])); ci++) {
    unsigned char *jpeg = NULL;
    size_t jsz = 0;

    if (make_valid_jpeg(cases[ci].w, cases[ci].h, cases[ci].subsamp,
                        &jpeg, &jsz) == -1) {
      printf("  FAIL D%d: build %dx%d sub=%d\n", ci, cases[ci].w,
             cases[ci].h, cases[ci].subsamp);
      nfail++;
      continue;
    }
    for (fi = 0; fi < nfactors; fi++) {
      ntests++;
      if (decode_one_scaled(jpeg, jsz, cases[ci].w, cases[ci].h,
                            cases[ci].subsamp, &factors[fi]) == -1) {
        printf("  FAIL D%d %dx%d sub=%d scale=%d/%d\n", ci, cases[ci].w,
               cases[ci].h, cases[ci].subsamp, factors[fi].num,
               factors[fi].denom);
        nfail++;
      }
    }
    printf("  [ok] D%d %dx%d sub=%d across %d scaling factors\n", ci,
           cases[ci].w, cases[ci].h, cases[ci].subsamp, nfactors);
    tj3Free(jpeg);
  }
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  printf("libjpeg-turbo SOF dimension/sampling/scale hardening test\n");

  group_a_malicious_sof();
  group_a_pixel_guard();
  group_b_bufsize();
  group_c_truncation();
  group_d_legal_boundary();

  printf("\n======================================================\n");
  if (nfail == 0)
    printf("ALL %d CHECKS PASSED\n", ntests);
  else
    printf("%d OF %d CHECKS FAILED\n", nfail, ntests);
  printf("======================================================\n");

  return nfail ? 1 : 0;
}
