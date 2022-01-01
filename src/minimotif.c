/*
 *   minimotif: A small DNA/RNA scanner for HOMER/JASPAR/MEME motifs
 *   Copyright (C) 2022  Benjamin Jean-Marie Tremblay
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <getopt.h>
#include <math.h>
#include <limits.h>
#include <time.h>

#define MINIMOTIF_VERSION     "1.0"
#define MINIMOTIF_YEAR         2022

/* These defaults can be safely changed. The only effects of doing so will be
 * on performance. Depending on whether your motifs are extremely large, or
 * you are working with extreme imbalances in your background, changing these
 * might be a good idea to get around internal size limits. Do be careful that
 * any changes don't lead to int overflows or going out of bounds.
 */

/* Max stored size of motif names.
 */
#define MAX_NAME_SIZE           256

/* The motif cannot be larger than 50 positions. This ensures no integer
 * overflow occurs if there are too many non-standard letters (as the current
 * solution to dealing with non-standard letters is to assign them a score of
 * -10,000,000; for 50 positions, this makes for a possible min score of
 * -500,000,000, or approx 1/4 of the way until INT_MIN [-2,147,483,648]).
 * This value is also used when performing memory allocation for new motifs,
 * regardless of the actual size of the motif (thus sticking to lower values
 * may be better for performance).
 */
#define MAX_MOTIF_SIZE          250    /* 5 rows per position */
#define AMBIGUITY_SCORE   -10000000

/* No bkg prob can be smaller than 0.001, to allow for a relatively small
 * max CDF size. (PWM scores are multiplied by 1000 and used as ints.)
 *     max score: (int) 1000*log2(1/0.001)      =>   9,965
 *     min score: (int) 1000*log2(0.001/0.997)  =>  -9,961
 *     cdf size:        (9965+9961)*50          => 996,300
 */
#define MIN_BKG_VALUE         0.001
#define MAX_CDF_SIZE        2097152
#define PWM_INT_MULTIPLIER   1000.0    /* Needs to be a double */

/* Max size of the parsed -b char array.
 */
#define USER_BKG_MAX_SIZE       256

/* Max size of the parsed MEME background probabilities.
 */
#define MEME_BKG_MAX_SIZE       256

/* Max size of PCM/PPM values in parsed motifs
 */
#define MOTIF_VALUE_MAX_CHAR    256

/* Max size of sequence names
 */
#define SEQ_NAME_MAX_CHAR       256

/* Size of progress bar
 */
#define PROGRESS_BAR_WIDTH      60
#define PROGRESS_BAR_STRING      \
  "============================================================"

/* Front-facing defaults.
 */
#define DEFAULT_NSITES         1000
#define DEFAULT_PVALUE      0.00001
#define DEFAULT_PSEUDOCOUNT       1

#define VEC_ADD(VEC, X, VEC_LEN)                                \
  do {                                                          \
    for (int Xi = 0; Xi < VEC_LEN; Xi++) VEC[Xi] += X;          \
  } while (0)

#define VEC_DIV(VEC, X, VEC_LEN)                                \
  do {                                                          \
    for (int Xi = 0; Xi < VEC_LEN; Xi++) VEC[Xi] /= X;          \
  } while (0)

#define VEC_SUM(VEC, VEC_SUM, VEC_LEN)                          \
  do {                                                          \
    for (int Xi = 0; Xi < VEC_LEN; Xi++) VEC_SUM += VEC[Xi];    \
  } while (0)

#define VEC_MIN(VEC, VEC_MIN, VEC_LEN)                          \
  do {                                                          \
    VEC_MIN = VEC[0];                                           \
    for (int Xi = 0; Xi < VEC_LEN; Xi++) {                      \
      if (VEC[Xi] < VEC_MIN) VEC_MIN = VEC[Xi];                 \
    }                                                           \
  } while (0)

#define ERASE_ARRAY(ARR, LEN)                                   \
  do {                                                          \
    for (int Xi = 0; Xi < LEN; Xi++) ARR[Xi] = 0;               \
  } while (0)

void usage() {
  printf(
    "minimotif v%s  Copyright (C) %d  Benjamin Jean-Marie Tremblay              \n"
    "                                                                              \n"
    "Usage:  minimotif [options] [ -m motifs.txt | -1 CONSENSUS ] -s sequences.fa  \n"
    "                                                                              \n"
    " -m <str>   Filename of text file containing motifs. Acceptable formats: MEME,\n"
    "            JASPAR, HOMER. Must be 1-%d bases wide.                           \n"
    " -1 <str>   Instead of -m, scan a single consensus sequence. Ambiguity letters\n"
    "            are allowed. Must be 1-%d bases wide. The -b, -t, -p and -n flags \n"
    "            are unused.                                                       \n"
    " -s <str>   Filename of fasta-formatted file containing DNA/RNA sequences to  \n"
    "            scan. Use '-' for stdin. Omitting -s will cause minimotif to print\n"
    "            the parsed motifs instead of scanning. Alternatively, solely      \n"
    "            providing -s and not -m/-1 will cause minimotif to return sequence\n"
    "            stats. Any spaces found are not read into the final scanned       \n"
    "            sequence. Non-standard characters (i.e. other than ACGTU) will be \n"
    "            read but are treated as gaps during scanning.                     \n"
    " -o <str>   Filename to output results. By default output goes to stdout.     \n"
    " -b <dbl>   Comma-separated background probabilities for A,C,G,T. By default  \n"
    "            the background probability values from the motif file (MEME only) \n"
    "            are used, or a uniform background is assumed. Used in PWM         \n"
    "            generation.                                                       \n"
    " -f         Only scan the forward strand.                                     \n"
    " -t <dbl>   Threshold P-value. Default: %g.                         \n"
    " -p <int>   Pseudocount for PWM generation. Default: %d. Must be a positive    \n"
    "            integer.                                                          \n"
    " -n <int>   Number of motif sites used in PWM generation. Default: %d.         \n"
    " -d         Deduplicate motif/sequence names. Default: abort. Duplicates will \n"
    "            have the motif/sequence and line numbers appended.                \n"
    " -r         Trim motif (JASPAR only) and sequence names to the first word.   \n"
    " -g         Print a progress bar during scanning. This turns off some of the  \n"
    "            messages printed by -w. Note that it's only useful if there is    \n"
    "            more than one input motif.                                        \n"
    " -v         Verbose mode. Recommended when using for the first time with new  \n"
    "            motifs/sequences, as warnings about potential issues will only be \n"
    "            printed when -v/-w are set.                                       \n"
    " -w         Very verbose mode. Only recommended for debugging purposes.       \n"
    " -h         Print this help message.                                          \n"
    , MINIMOTIF_VERSION, MINIMOTIF_YEAR, MAX_MOTIF_SIZE / 5, MAX_MOTIF_SIZE / 5,
      DEFAULT_PVALUE, DEFAULT_PSEUDOCOUNT, DEFAULT_NSITES
  );
}

const unsigned char char2index[] = {
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 0, 4, 1, 4, 4, 4, 2, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 0, 4, 1, 4, 4, 4, 2, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4 
};

int char_counts[] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

const double consensus2probs[] = {
  1.0,   0.0,   0.0,   0.0,        /*  0. A */
  0.0,   1.0,   0.0,   0.0,        /*  1. C */
  0.0,   0.0,   1.0,   0.0,        /*  2. G */
  0.0,   0.0,   0.0,   1.0,        /*  3. T */
  0.0,   0.5,   0.0,   0.5,        /*  4. Y */
  0.5,   0.0,   0.5,   0.0,        /*  5. R */
  0.5,   0.0,   0.0,   0.5,        /*  6. W */
  0.0,   0.5,   0.5,   0.0,        /*  7. S */
  0.0,   0.0,   0.5,   0.5,        /*  8. K */
  0.5,   0.5,   0.0,   0.0,        /*  9. M */
  0.333, 0.0,   0.333, 0.333,      /* 10. D */
  0.333, 0.333, 0.333, 0.0,        /* 11. V */
  0.333, 0.333, 0.0,   0.333,      /* 12. H */
  0.0,   0.333, 0.333, 0.333,      /* 13. B */
  0.25,  0.25,  0.25,  0.25        /* 14. N */
};

const int consensus2index[] = {
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1,  0, 13,  1, 10, -1, -1,  2, 12, -1, -1,  8, -1,  9, 14, -1,
  -1, -1,  5,  7,  3,  3, 11,  6, -1,  4, -1, -1, -1, -1, -1, -1,
  -1,  0, 13,  1, 10, -1, -1,  2, 12, -1, -1,  8, -1,  9, 14, -1,
  -1, -1,  5,  7,  3,  3, 11,  6, -1,  4, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

enum MOTIF_FMT {
  FMT_MEME    = 1,
  FMT_HOMER   = 2,
  FMT_JASPAR  = 3,
  FMT_UNKNOWN = 4
};

typedef struct args_t {
  double   bkg[4];
  double   pvalue;
  int      nsites;
  int      pseudocount; 
  int      scan_rc;
  int      dedup;
  int      trim_names;
  int      use_user_bkg;
  int      progress;
  int      v;
  int      w;
} args_t;

args_t args = {
  .bkg             = {0.25, 0.25, 0.25, 0.25},
  .pvalue          = DEFAULT_PVALUE,
  .nsites          = DEFAULT_NSITES,
  .pseudocount     = DEFAULT_PSEUDOCOUNT,
  .scan_rc         = 1,
  .dedup           = 0,
  .trim_names      = 0,
  .use_user_bkg    = 0,
  .progress        = 0,
  .v               = 0,
  .w               = 0
};

typedef struct motif_t {
  int       size;
  int       threshold;
  int       min;                         /* Smallest single PWM score */
  int       max;                         /* Largest single PWM score  */
  int       max_score;                   /* Largest total PWM score   */
  int       min_score;                   /* Smallest total PWM score  */
  int       cdf_size;
  int       file_line_num;
  char      name[MAX_NAME_SIZE];
  int       pwm[MAX_MOTIF_SIZE];
  int       pwm_rc[MAX_MOTIF_SIZE];
  double   *cdf;
} motif_t;

motif_t **motifs;

typedef struct motif_info_t {
  int fmt;
  int n;
} motif_info_t;

motif_info_t motif_info = {
  .fmt = 0,
  .n = 0
};

typedef struct seq_info_t {
  int     n;
  int     total_bases;
  int     unknowns;
  double  gc_pct;
} seq_info_t;

seq_info_t seq_info = {
  .n = 0,
  .total_bases = 0,
  .unknowns = 0,
  .gc_pct = 0.0
};

char            **seq_names;
unsigned char   **seqs;
int              *seq_sizes;
int              *seq_line_nums;

void free_seqs(void) {
  for (int i = 0; i < seq_info.n; i++) {
    free(seq_names[i]);
    free(seqs[i]);
  }
  free(seq_names);
  free(seq_sizes);
  free(seq_line_nums);
  free(seqs);
}

void free_motifs(void) {
  for (int i = 0; i < motif_info.n; i++) {
    if (motifs[i]->cdf_size) free(motifs[i]->cdf);
    free(motifs[i]);
  }
  free(motifs);
}

typedef struct files_t {
  FILE   *m;
  FILE   *s;
  FILE   *o;
  int     m_open;
  int     s_open;
  int     o_open;
} files_t;

files_t files = { .m_open = 0, .s_open = 0, .o_open = 0 };

void close_files(void) {
  if (files.m_open) fclose(files.m);
  if (files.s_open) fclose(files.s);
  if (files.o_open) fclose(files.o);
}

void init_motif(motif_t *motif) {
  ERASE_ARRAY(motif->name, MAX_NAME_SIZE);
  motif->name[0] = 'm'; motif->name[1] = 'o';
  motif->name[2] = 't'; motif->name[3] = 'i';
  motif->name[4] = 'f';
  motif->size = 0; motif->threshold = 0; motif->max_score = 0;
  motif->min = 0; motif->max = 0; motif->cdf_size = 0;
  motif->file_line_num = 0; motif->min_score = 0;
  for (int i = 0; i < MAX_MOTIF_SIZE; i++) {
    motif->pwm[i] = 0;
    motif->pwm_rc[i] = 0;
  }
  for (int i = 4; i < MAX_MOTIF_SIZE; i += 5) {
    motif->pwm[i] = AMBIGUITY_SCORE;
    motif->pwm_rc[i] = AMBIGUITY_SCORE;
  }
}

static inline void set_score(motif_t *motif, const unsigned char let, const int pos, const int score) {
  motif->pwm[char2index[let] + pos * 5] = score;
}

static inline int get_score(const motif_t *motif, const unsigned char let, const int pos) {
  return motif->pwm[char2index[let] + pos * 5];
}

static inline int get_score_i(const motif_t *motif, const int i, const int pos) {
  return motif->pwm[i + pos * 5];
}

static inline void set_score_rc(motif_t *motif, const unsigned char let, const int pos, const int score) {
  motif->pwm_rc[char2index[let] + pos * 5] = score;
}

static inline int get_score_rc(const motif_t *motif, const unsigned char let, const int pos) {
  return motif->pwm_rc[char2index[let] + pos * 5];
}

void badexit(const char *msg) {
  fprintf(stderr, "%s\nRun minimotif -h to see usage.\n", msg);
  free_motifs();
  free_seqs();
  close_files();
  exit(EXIT_FAILURE);
}

void fill_cdf(motif_t *motif) {
  int max_score = motif->max - motif->min;
  int pdf_size = motif->size * max_score + 1;
  motif->cdf_size = pdf_size;
  int max_step, s;
  double pdf_sum = 0.0;
  if (args.w) fprintf(stderr, "    Generating CDF for [%s] (n=%'d) ... ",
      motif->name, pdf_size);
  if (pdf_size > MAX_CDF_SIZE) {
    if (args.w) fprintf(stderr, "\n");
    fprintf(stderr,
        "Internal error: Requested CDF size for [%s] is too large (%'d>%'d).\n",
        motif->name, pdf_size, MAX_CDF_SIZE);
    fprintf(stderr, "    Make sure no background values are below %f.",
        MIN_BKG_VALUE);
    badexit("");
  }
  double *tmp_pdf = malloc(pdf_size * sizeof(double));
  if (tmp_pdf == NULL) {
    badexit("Error: Memory allocation for motif PDF failed.");
  }
  motif->cdf = malloc(pdf_size * sizeof(double));
  if (motif->cdf == NULL) {
    free(tmp_pdf);
    badexit("Error: Memory allocation for motif CDF failed.");
  }
  for (int i = 0; i < pdf_size; i++) motif->cdf[i] = 1.0;
  for (int i = 0; i < motif->size; i++) {
    max_step = i * max_score;
    for (int j = 0; j < pdf_size; j++) {
      tmp_pdf[j] = motif->cdf[j];
    }
    for (int j = 0; j < max_step + max_score + 1; j++) {
      motif->cdf[j] = 0.0;
    }
    for (int j = 0; j < 4; j++) {
      s = get_score_i(motif, j, i) - motif->min;
      for (int k = 0; k <= max_step; k++) {
        if (tmp_pdf[k] != 0.0) {
          motif->cdf[k+s] = motif->cdf[k+s] + tmp_pdf[k] * args.bkg[j];
        }
      }
    }
  }
  free(tmp_pdf);
  for (int i = 0; i < pdf_size; i++) pdf_sum += motif->cdf[i];
  if (fabs(pdf_sum - 1.0) > 0.0001) {
    if (args.w) {
      fprintf(stderr, "Internal warning: sum(PDF)!= 1.0 for [%s] (sum=%.2g)\n",
          motif->name, pdf_sum);
    }
    for (int i = 0; i < pdf_size; i++) {
      motif->cdf[i] /= pdf_sum;
    }
  }
  for (int i = pdf_size - 2; i >= 0; i--) {
    motif->cdf[i] += motif->cdf[i + 1];
  }
  if (args.w) fprintf(stderr, "done.\n");
}

static inline double score2pval(const motif_t *motif, const int score) {
  return motif->cdf[score - motif->min * motif->size];
}

void set_threshold(motif_t *motif) {
  int threshold_i = motif->cdf_size;
  for (int i = 0; i < motif->cdf_size; i++) {
    if (motif->cdf[i] < args.pvalue) {
      threshold_i = i;
      break;
    }
  }
  motif->threshold -= motif->min;
  motif->threshold *= motif->size;
  motif->threshold = threshold_i - motif->threshold;
  for (int i = 0; i < motif->size; i++) {
    int max_pos = get_score_i(motif, 0, i);
    int min_pos = max_pos;
    for (int j = 1; j < 4; j++) {
      int tmp_pos = get_score_i(motif, j, i);
      if (tmp_pos > max_pos) max_pos = tmp_pos;
      if (tmp_pos < min_pos) min_pos = tmp_pos;
    }
    motif->max_score += max_pos;
    motif->min_score += min_pos;
  }
  double min_pvalue = score2pval(motif, motif->max_score);
  if (min_pvalue / args.pvalue > 1.0001) {
    if (args.w) {
      fprintf(stderr,
        "Warning: Min possible pvalue for [%s] is greater than the threshold,\n",
        motif->name);
      fprintf(stderr, "    motif will not be scored (%g>%g).\n",
        min_pvalue, args.pvalue);
    }
    motif->threshold = INT_MAX;
  }
}

int check_and_load_bkg(double *bkg) {
  if (bkg[0] == -1.0 || bkg[1] == -1.0 || bkg[2] == -1.0 || bkg[3] == -1.0) {
    fprintf(stderr, "Error: Too few background values found (need 4)."); return 1;
  }
  double min = 0; VEC_MIN(bkg, min, 4);
  if (min < MIN_BKG_VALUE) {
    if (args.v) {
      fprintf(stderr,
        "Warning: Detected background values smaller than allowed min,\n");
      fprintf(stderr, "    adjusting (%.2g<%.2g).\n", min, MIN_BKG_VALUE);
    }
    VEC_ADD(bkg, MIN_BKG_VALUE, 4);
  }
  double sum = bkg[0] + bkg[1] + bkg[2] + bkg[3];
  if (fabs(sum - 1.0) > 0.001 && args.v) {
    fprintf(stderr,
      "Warning: Background values don't add up to 1.0, adjusting (sum=%.3g).\n",
      sum);
  }
  VEC_DIV(bkg, sum, 4);
  args.bkg[0] = bkg[0]; args.bkg[1] = bkg[1]; args.bkg[2] = bkg[2]; args.bkg[3] = bkg[3];
  return 0;
}

void parse_user_bkg(const char *bkg_usr) {
  int i = 0, j = 0, bi = 0;
  char bc[USER_BKG_MAX_SIZE];
  double b[] = {-1.0, -1.0, -1.0, -1.0};
  ERASE_ARRAY(bc, USER_BKG_MAX_SIZE);
  while (bkg_usr[i] != '\0') {
    if (bkg_usr[i] != ',' && bkg_usr[i] != ' ') {
      bc[j] = bkg_usr[i];
      j++;
    } else if (bkg_usr[i] == ',') {
      if (bi > 2) {
        badexit("Error: Too many background values provided (need 4).");
      }
      b[bi] = atof(bc);
      ERASE_ARRAY(bc, USER_BKG_MAX_SIZE);
      bi++;
      j = 0;
    }
    i++;
  }
  b[3] = atof(bc);
  if (check_and_load_bkg(b)) badexit("");
  if (args.w) {
    fprintf(stderr, "Using new background values:\n");
    fprintf(stderr, "    A=%.3g", args.bkg[0]);
    fprintf(stderr, "    C=%.3g\n", args.bkg[1]);
    fprintf(stderr, "    G=%.3g", args.bkg[2]);
    fprintf(stderr, "    T=%.3g\n", args.bkg[3]);
  }
}

/*
double b2kb(const int x) {
  return x / 1024.0;
}
*/

static inline double b2mb(const int x) {
  return (x / 1024.0) / 1024.0;
}

/*
int calc_motif_size(const motif_t *motif) {
  return sizeof(motif_t) + sizeof(double) * motif->cdf_size;
}
*/

int check_line_contains(const char *line, const char *substring) {
  int sublen = 0;
  for (int i = 0; i < INT_MAX; i++) {
    if (substring[i] == '\0') {
      sublen = i; break;
    }
  }
  for (int i = 0; i < sublen; i++) {
    if (line[i] == '\0' || line[i] != substring[i]) return 0;
  }
  return 1;
}

int count_nonempty_chars(const char *line) {
  int total_chars = 0, i = 0;
  while (line[i] != '\0') {
    if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') {
      total_chars++;
    }
    i++;
  }
  return total_chars;
}

int detect_motif_fmt(void) {
  int jaspar_or_homer = 0, file_fmt = 0;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  while ((read = getline(&line, &len, files.m)) != -1) {
    if (!count_nonempty_chars(line)) continue;
    if (check_line_contains(line, "MEME version \0")) {
      if (args.w) {
        fprintf(stderr, "Detected MEME format (version %c).\n", line[13]);
      }
      file_fmt = FMT_MEME;
      break;
    }
    if (jaspar_or_homer) {
      if (line[0] == '0' || line[0] == '1') {
        file_fmt = FMT_HOMER;
        if (args.w) fprintf(stderr, "Detected HOMER format.\n");
        break;
      } else if (line[0] == 'A') {
        file_fmt = FMT_JASPAR;
        if (args.w) fprintf(stderr, "Detected JASPAR format.\n");
        break;
      }
    } else if (line[0] == '>') {
      jaspar_or_homer = 1;
    }
  }
  rewind(files.m);
  free(line);
  if (!file_fmt) file_fmt = FMT_UNKNOWN;
  return file_fmt;
}

int add_motif() {
  motif_t **tmp_ptr = realloc(motifs, sizeof(*motifs) * (motif_info.n + 1));
  if (tmp_ptr == NULL) {
    fprintf(stderr, "Error: Failed to allocate memory for motifs.");
    return 1;
  } else {
    motifs = tmp_ptr;
  }
  motifs[motif_info.n] = malloc(sizeof(motif_t));
  if (motifs[motif_info.n] == NULL) {
    fprintf(stderr, "Error: Failed to allocate memory for motifs.");
    return 1;
  }
  motif_info.n++;
  init_motif(motifs[motif_info.n - 1]);
  return 0;
}

int check_char_is_one_of(const char c, const char *list) {
  int i = 0;
  for (;;) {
    if (list[i] == '\0') break;
    if (list[i] == c) return 1;
    i++;
  }
  return 0;
}

int calc_score(const double prob_i, const double bkg_i) {
  double x;
  x = prob_i * args.nsites;
  x += ((double) args.pseudocount) / 4.0;
  x /= (double) (args.nsites + args.pseudocount);
  return (int) (log2(x / bkg_i) * PWM_INT_MULTIPLIER);
}

int normalize_probs(double *probs, const char *name) {
  double sum = probs[0] + probs[1] + probs[2] + probs[3];
  if (fabs(sum - 1.0) > 0.1) {
    if (args.w) fprintf(stderr, "\n");
    fprintf(stderr,
      "Error: Position for [%s] does not add up to 1 (sum=%.3g)",
      name, sum);
    return 1;
  }
  if (fabs(sum - 1.0) > 0.02) {
    if (args.w) {
      fprintf(stderr,
        "\nWarning: Position for [%s] does not add up to 1, adjusting (sum=%.3g) ",
        name, sum);
    }
    VEC_DIV(probs, sum, 4);
  }
  return 0;
}

int get_line_probs(const motif_t *motif, const char *line, double *probs, const int n) {
  int i = 0, j = 0, which_i = -1, prev_line_was_space = 1;
  char pos_i[MOTIF_VALUE_MAX_CHAR];
  ERASE_ARRAY(pos_i, MOTIF_VALUE_MAX_CHAR);
  for (;;) {
    if (line[i] != ' ' && line[i] != '\t') break;
    i++;
  }
  while (line[i] != '\0' && line[i] != '\r' && line[i] != '\n') {
    if (line[i] != ' ' && line[i] != '\t') {
      pos_i[j] = line[i];
      j++;
      prev_line_was_space = 0;
    } else {
      if (!prev_line_was_space) {
        which_i++; 
        if (which_i > n - 1) {
          if (args.w) fprintf(stderr, "\n");
          fprintf(stderr,
            "Error: Motif [%s] has too many columns (need %d).",
            motif->name, n); return 1;
        }
        probs[which_i] = atof(pos_i);
        ERASE_ARRAY(pos_i, MOTIF_VALUE_MAX_CHAR);
        j = 0;
      }
      prev_line_was_space = 1;
    }
    i++;
  }

  if (!prev_line_was_space) {
    which_i++; 
    if (which_i > n - 1) {
      if (args.w) fprintf(stderr, "\n");
      fprintf(stderr,
        "Error: Motif [%s] has too many columns (need %d).",
        motif->name, n); return 1;
    }
    probs[which_i] = atof(pos_i);
  }

  if (which_i == -1) {
    if (args.w) fprintf(stderr, "\n");
    fprintf(stderr, "Error: Motif [%s] has an empty row.",
      motif->name); return 1;
  }

  if (which_i < n - 1) {
    if (args.w) fprintf(stderr, "\n");
    fprintf(stderr, "Error: Motif [%s] has too few columns (need %d).",
      motif->name, n); return 1;
  }

  return 0;
}

int add_motif_column(motif_t *motif, const char *line, const int pos) {
  double probs[] = {-1.0, -1.0, -1.0, -1.0};
  if (get_line_probs(motif, line, probs, 4)) return 1;
  if (normalize_probs(probs, motif->name)) return 1;
  set_score(motif, 'A', pos, calc_score(probs[0], args.bkg[0]));
  set_score(motif, 'C', pos, calc_score(probs[1], args.bkg[1]));
  set_score(motif, 'G', pos, calc_score(probs[2], args.bkg[2]));
  set_score(motif, 'T', pos, calc_score(probs[3], args.bkg[3]));
  return 0;
}

int check_meme_alph(const char *line, const int line_num) {
  if (check_line_contains(line, "ALPHABET= ACDEFGHIKLMNPQRSTVWY\0")) {
    fprintf(stderr, "Error: Detected protein alphabet (L%d).", line_num);
    return 1;
  }
  return 0;
}

int check_meme_strand(const char *line, const int line_num) {
  int scan_fwd = 0, scan_rev = 0, i = 0;
  for (;;) {
    if (line[i] == '\0') break;
    if (line[i] == '+') scan_fwd++;
    if (line[i] == '-') scan_rev++;
    i++;
  }
  if (((scan_fwd > 1 || scan_rev > 1) || (!scan_fwd && !scan_rev)) && args.v) {
    fprintf(stderr, "Warning: Possible malformed strand field (L%d).\n", line_num);
  }
  if (args.scan_rc && scan_fwd && !scan_rev && args.v) {
    fprintf(stderr, "Warning: MEME motifs are only for the forward strand (L%d).\n",
      line_num);
  }
  if (!scan_fwd && scan_rev && args.v) {
    fprintf(stderr, "Warning: MEME motifs are only for the reverse strand (L%d).\n",
      line_num);
  }
  if (!args.scan_rc && scan_fwd && scan_rev && args.v) {
    fprintf(stderr, "Warning: MEME motifs are for both strands (L%d).\n",
      line_num);
  }
  return 0;
}

int get_meme_bkg(const char *line, const int line_num) {
  if (args.use_user_bkg) return 0;
  double bkg_probs[] = {-1.0, -1.0, -1.0, -1.0};
  int i = 1, let_i = 0, j = 0, empty = 0;
  char bkg_char[MEME_BKG_MAX_SIZE];
  ERASE_ARRAY(bkg_char, MEME_BKG_MAX_SIZE);
  if (line[0] != 'A') {
    fprintf(stderr, "Error: Expected first character of background line to be 'A' (L%d).",
      line_num); return 1;
  }
  while (line[i] != '\0' && line[i] != '\n' && line[i] != '\r') {
    if (let_i > 3) {
      fprintf(stderr, "Error: Parsed too many background values in MEME file (L%d).",
        line_num); return 1;
    }
    if (line[i] != ' ' && line[i] != '\t') {
      if (line[i] == 'C') {
        if (!empty) {
          fprintf(stderr, "Error: Expected whitespace before 'C' character (L%d).",
            line_num); return 1;
        }
        if (let_i != 0) {
          fprintf(stderr,
            "Error: Expected 'C' to be second letter in MEME background (L%d).",
            line_num); return 1;
        }
        bkg_probs[let_i] = atof(bkg_char);
        ERASE_ARRAY(bkg_char, MEME_BKG_MAX_SIZE);
        let_i = 1; j = 0;
      } else if (line[i] == 'G') {
        if (!empty) {
          fprintf(stderr, "Error: Expected whitespace before 'C' character (L%d).",
            line_num); return 1;
        }
        if (let_i != 1) {
          fprintf(stderr,
            "Error: Expected 'G' to be third letter in MEME background (L%d).",
            line_num); return 1;
        }
        bkg_probs[let_i] = atof(bkg_char);
        ERASE_ARRAY(bkg_char, MEME_BKG_MAX_SIZE);
        let_i = 2; j = 0;
      } else if (line[i] == 'T' || line[i] == 'U') {
        if (!empty) {
          fprintf(stderr, "Error: Expected whitespace before 'C' character (L%d).",
            line_num); return 1;
        }
        if (let_i != 2) {
          fprintf(stderr,
            "Error: Expected 'T/U' to be fourth letter in MEME background (L%d).",
            line_num); return 1;
        }
        bkg_probs[let_i] = atof(bkg_char);
        ERASE_ARRAY(bkg_char, MEME_BKG_MAX_SIZE);
        let_i = 3; j = 0;
      } else if (check_char_is_one_of(line[i], "0123456789.\0")) {
        bkg_char[j] = line[i]; 
        j++;
      } else {
        fprintf(stderr,
          "Error: Encountered unexpected character (%c) in MEME background (L%d).",
          line[i], line_num); return 1;
      }
      empty = 0;
    } else {
      empty = 1;
    }
    i++;
  }
  if (bkg_char[0] != '\0') bkg_probs[let_i] = atof(bkg_char);
  if (check_and_load_bkg(bkg_probs)) return 1;
  if (args.w) {
    fprintf(stderr, "Found MEME background values:\n");
    fprintf(stderr, "    A=%.3g", args.bkg[0]);
    fprintf(stderr, "    C=%.3g\n", args.bkg[1]);
    fprintf(stderr, "    G=%.3g", args.bkg[2]);
    fprintf(stderr, "    T=%.3g\n", args.bkg[3]);
  }
  return 0;
}

void parse_meme_name(const char *line, const int motif_i) {
  int i = 5, j = 0, name_read = 0;
  while (line[i] != '\0' && line[i] != '\r' && line[i] != '\n') {
    if (line[i] == ' ' && name_read) break;
    else if (line[i] == ' ') {
      i++;
      continue;
    }
    name_read = 1;
    motifs[motif_i]->name[j] = line[i];
    j++; i++;
  }
  if (args.w) fprintf(stderr, "    Found motif: %s (size=", motifs[motif_i]->name);
}

void read_meme(void) {
  motif_info.fmt = FMT_MEME;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  int line_num = 0, alph_detected = 0, strand_detected = 0, l_p_m_L = 0;
  int motif_i = -1, pos_i = -1, live_motif = 0, bkg_let_freqs_L = 0;
  while ((read = getline(&line, &len, files.m)) != -1) {
    line_num++;
    if (check_line_contains(line, "Background letter frequencies\0")) {
      if (bkg_let_freqs_L) {
        free(line);
        fprintf(stderr,
          "Error: Detected multiple background definition lines in MEME file (L%d).",
          line_num);
      } else {
        if (motif_i >= 0) {
          free(line);
          fprintf(stderr, "Error: Found background definition line after motifs (L%d).",
            line_num);
          badexit("");
        }
        bkg_let_freqs_L = line_num;
      }
    } else if (bkg_let_freqs_L && bkg_let_freqs_L == line_num - 1) {
      if (get_meme_bkg(line, line_num)) {
        free(line);
        badexit("");
      }
    } else if (check_line_contains(line, "ALPHABET\0")) {
      if (alph_detected) {
        free(line);
        fprintf(stderr,
          "Error: Detected multiple alphabet definition lines in MEME file (L%d).",
          line_num);
        badexit("");
      }
      if (motif_i >= 0) {
        free(line);
        fprintf(stderr, "Error: Found alphabet definition line after motifs (L%d).",
          line_num);
        badexit("");
      }
      if (check_meme_alph(line, line_num)) {
        free(line);
        badexit("");
      }
      alph_detected = 1;
    } else if (check_line_contains(line, "strands:\0")) {
      if (strand_detected) {
        free(line);
        fprintf(stderr,
          "Error: Detected multiple strand information lines in MEME file (L%d).",
          line_num);
        badexit("");
      }
      if (motif_i >= 0) {
        free(line);
        fprintf(stderr, "Error: Found strand information line after motifs (L%d).",
          line_num);
        badexit("");
      }
      if (check_meme_strand(line, line_num)) {
        free(line);
        badexit("");
      }
      strand_detected = 1;
    } else if (check_line_contains(line, "MOTIF\0")) {
      if (motif_i >= 0 && args.w) fprintf(stderr, "%d)\n", motifs[motif_i]->size);
      motif_i++;
      if (add_motif()) {
        free(line);
        badexit("");
      }
      motifs[motif_i]->file_line_num = line_num;
      parse_meme_name(line, motif_i);
      pos_i = 0;
    } else if (check_line_contains(line, "letter-probability matrix\0")) {
      if (pos_i != 0) {
        free(line);
        fprintf(stderr, "Error: Possible malformed MEME motif (L%d).",
          line_num);
        badexit("");
      }
      l_p_m_L = line_num;
      live_motif = 1;
    } else if (live_motif) {

      if (!count_nonempty_chars(line) || check_char_is_one_of('-', line) ||
          check_char_is_one_of('*', line)) {
        live_motif = 0;
      } else if (line_num == (l_p_m_L + pos_i + 1)) {

        if (pos_i >= MAX_MOTIF_SIZE / 5) {
          free(line);
          fprintf(stderr, "Error: Motif [%s] is too large (max=%d)",
            motifs[motif_i]->name, MAX_MOTIF_SIZE / 5);
          badexit("");
        }
        if (add_motif_column(motifs[motif_i], line, pos_i)) {
          free(line);
          badexit("");
        }
        pos_i++;
        motifs[motif_i]->size = pos_i;

      } else {
        live_motif = 0;
      }

    }
  }
  free(line);
  if (motif_i >= 0 && args.w) fprintf(stderr, "%d)\n", motifs[motif_i]->size);
  if (!motif_info.n) badexit("Error: Failed to detect any motifs in MEME file.");
  if (args.v) {
    fprintf(stderr, "Found %'d MEME motif(s).\n", motif_info.n);
  }
}

void parse_homer_name(const char *line, const int motif_i) {
  int name_start = 0, name_end = 0, i = 1, in_between = 0, j = 0;
  while (line[i] != '\0' && line[i] != '\r' && line[i] != '\n') {
    if (line[i] == '\t') {
      if (name_start) {
        name_end = i;
        break;
      } else {
        in_between = 1;
      }
    } else if (in_between) {
      if (!name_start) name_start = i;
    } 
    i++;
  }
  if (!name_start) {
    if (args.w) {
      fprintf(stderr, "Warning: Failed to parse motif name [#%'d].\n", motif_i + 1);
    }
  } else if (!name_end) {
    if (args.w) {
      fprintf(stderr, "Warning: HOMER motif is missing logodds score [#%'d].\n", 
        motif_i + 1);
    }
    name_end = i;
  }
  for (int k = name_start; k < name_end; k++) {
    motifs[motif_i]->name[j] = line[k];
    j++;
  }
  if (args.w) fprintf(stderr, "    Found motif: %s (size=", motifs[motif_i]->name);
}

void read_homer(void) {
  motif_info.fmt = FMT_HOMER;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  int motif_i = -1, line_num = 0, pos_i;
  while ((read = getline(&line, &len, files.m)) != -1) {
    line_num++;
    if (line[0] == '>') {
      if (motif_i >= 0 && args.w) fprintf(stderr, "%d)\n", motifs[motif_i]->size);
      motif_i++;
      if (add_motif()) {
        free(line);
        badexit("");
      }
      motifs[motif_i]->file_line_num = line_num;
      parse_homer_name(line, motif_i);
      pos_i = 0;
    } else if (count_nonempty_chars(line)) {
      if (pos_i > MAX_MOTIF_SIZE / 5) {
        fprintf(stderr, "Error: Motif [%s] is too large (max=%'d).\n",
          motifs[motif_i]->name, MAX_MOTIF_SIZE / 5);
      }
      if (add_motif_column(motifs[motif_i], line, pos_i)) {
        free(line);
        badexit("");
      }
      pos_i++;
      motifs[motif_i]->size = pos_i;
    }
  }
  free(line);
  if (motif_i >= 0 && args.w) fprintf(stderr, "%d)\n", motifs[motif_i]->size);
  if (args.v) {
    fprintf(stderr, "Found %'d HOMER motif(s).\n", motif_info.n);
  }
}

int get_pwm_max(const motif_t *motif) {
  int max = 0, val;
  for (int pos = 0; pos < motif->size; pos++) {
    for (int let = 0; let < 4; let++) {
      val = get_score_i(motif, let, pos);
      if (val > max) max = val;
    }
  }
  return max;
}

int get_pwm_min(const motif_t *motif) {
  int min = 0, val;
  for (int pos = 0; pos < motif->size; pos++) {
    for (int let = 0; let < 4; let++) {
      val = get_score_i(motif, let, pos);
      if (val < min) min = val;
    }
  }
  return min;
}

void fill_pwm_rc(motif_t *motif) {
  for (int pos = 0; pos < motif->size; pos++) {
    set_score_rc(motif, 'A', motif->size - 1 - pos, get_score(motif, 'T', pos));
    set_score_rc(motif, 'C', motif->size - 1 - pos, get_score(motif, 'G', pos));
    set_score_rc(motif, 'G', motif->size - 1 - pos, get_score(motif, 'C', pos));
    set_score_rc(motif, 'T', motif->size - 1 - pos, get_score(motif, 'A', pos));
  }
}

void complete_motifs() {
  for (int i = 0; i < motif_info.n; i++) {
    motifs[i]->min = get_pwm_min(motifs[i]);
    motifs[i]->max = get_pwm_max(motifs[i]);
    fill_pwm_rc(motifs[i]);
  }
}

void print_motif(motif_t *motif, const int n) {
  /* fprintf(files.o, "Motif: %s (%'.2f KB)\n", motif->name, */
  /*   b2kb(calc_motif_size(motif))); */
  fprintf(files.o, "Motif: %s (N%d L%d)\n", motif->name, n, motif->file_line_num);
  if (motif->threshold == INT_MAX) {
    fprintf(files.o, "MaxScore=%.2f\tThreshold=%s\n",
      motif->max_score / PWM_INT_MULTIPLIER, "[exceeds max]");
  } else {
    fprintf(files.o, "MaxScore=%.2f\tThreshold=%.2f\n",
      motif->max_score / PWM_INT_MULTIPLIER, motif->threshold / PWM_INT_MULTIPLIER);
  }
  fprintf(files.o, "Motif PWM:\n\tA\tC\tG\tT\n");
  for (int i = 0; i < motif->size; i++) {
    fprintf(files.o, "%d:\t%.2f\t%.2f\t%.2f\t%.2f\n", i + 1,
      get_score(motif, 'A', i) / PWM_INT_MULTIPLIER,
      get_score(motif, 'C', i) / PWM_INT_MULTIPLIER,
      get_score(motif, 'G', i) / PWM_INT_MULTIPLIER,
      get_score(motif, 'T', i) / PWM_INT_MULTIPLIER);
  }
  /*
  fprintf(files.o, "Reverse complement:\n\tA\tC\tG\tT\n");
  for (int i = 0; i < motif->size; i++) {
    fprintf(files.o, "%d:\t%.2f\t%.2f\t%.2f\t%.2f\n", i + 1,
      get_score_rc(motif, 'A', i) / PWM_INT_MULTIPLIER,
      get_score_rc(motif, 'C', i) / PWM_INT_MULTIPLIER,
      get_score_rc(motif, 'G', i) / PWM_INT_MULTIPLIER,
      get_score_rc(motif, 'T', i) / PWM_INT_MULTIPLIER);
  }
  */
  fprintf(files.o, "Score=%.2f\t-->     p=1\n",
      motif->min_score / PWM_INT_MULTIPLIER);
  fprintf(files.o, "Score=%.2f\t-->     p=%.2g\n",
      (motif->min_score / 2) / PWM_INT_MULTIPLIER,
      score2pval(motif, motif->min_score / 2));
  fprintf(files.o, "Score=0.00\t-->     p=%.2g\n",
      score2pval(motif, 0.0));
  fprintf(files.o, "Score=%.2f\t-->     p=%.2g\n",
      (motif->max_score / 2) / PWM_INT_MULTIPLIER,
      score2pval(motif, motif->max_score / 2));
  fprintf(files.o, "Score=%.2f\t-->     p=%.2g\n",
      motif->max_score / PWM_INT_MULTIPLIER,
      score2pval(motif, motif->max_score));
}

void parse_jaspar_name(const char *line, const int motif_i) {
  int i = 0, j = 1;
  for (;;) {
    if (line[j] == '\r' || line[j] == '\n' || line[j] == '\0') break;
    motifs[motif_i]->name[i] = line[j];
    i++; j++;
  }
  motifs[motif_i]->name[i] = '\0';
  if (args.w) fprintf(stderr, "    Found motif: %s (size=", motifs[motif_i]->name);
}

int add_jaspar_row(motif_t *motif, const char *line) {
  int row_i = -1, i = 0, left_bracket = -1, right_bracket = -1;
  char let;
  for (;;) {
    if (line[i] == '\r' || line[i] == '\n' || line[i] == '\0') break;
    switch (line[i]) {
      case '\t':
      case ' ':
        break;
      case 'a':
      case 'A':
        row_i = 0;
        let = 'A';
        break;
      case 'c':
      case 'C':
        row_i = 1;
        let = 'C';
        break;
      case 'g':
      case 'G':
        row_i = 2;
        let = 'G';
        break;
      case 'u':
      case 'U':
      case 't':
      case 'T':
        row_i = 3;
        let = 'T';
        break;
      case '[':
        left_bracket = i;
        break;
      case ']':
        right_bracket = i;
        break;
    }
    i++;
  }
  if (row_i == -1) {
    fprintf(stderr, "Error: Couldn't find ACGTU in motif [%s] row names.", motif->name);
    return 1;
  }
  if (left_bracket == -1 || right_bracket == -1) {
    fprintf(stderr, "Error: Couldn't find '[]' in motif [%s] row (%d).",
        motif->name, row_i + 1);
    return 1;
  }
  int pos_i = -1, prev_line_was_space = 1, k = 0;
  char prob_c[MOTIF_VALUE_MAX_CHAR];
  ERASE_ARRAY(prob_c, MOTIF_VALUE_MAX_CHAR);
  i = left_bracket + 1;
  for (;;) {
    if (line[i] != ' ' && line[i] != '\t') break;
    i++;
  }
  for (int j = i; j < right_bracket; j++) {
    if (line[j] != ' ' && line[j] != '\t') {
      prob_c[k] = line[j];
      k++; prev_line_was_space = 0;
    } else {
      if (!prev_line_was_space) {
        pos_i++;
        if (pos_i + 1 > MAX_MOTIF_SIZE) {
          fprintf(stderr, "Error: Motif [%s] has too many columns (need %d).",
            motif->name, MAX_MOTIF_SIZE); return 1;
        }
        set_score(motif, let, pos_i, atoi(prob_c));
        ERASE_ARRAY(prob_c, MOTIF_VALUE_MAX_CHAR);
        k = 0;
      }
      prev_line_was_space = 1;
    }
  }
  if (!prev_line_was_space) {
    pos_i++;
    if (pos_i > MAX_NAME_SIZE) {
      fprintf(stderr, "Error: Motif [%s] has too many columns (need %d).",
        motif->name, MAX_MOTIF_SIZE); return 1;
    }
    set_score(motif, let, pos_i, atoi(prob_c));
  }
  if (pos_i == -1) {
    fprintf(stderr, "Error: Motif [%s] has an empty row.", motif->name); return 1;
  }
  pos_i++;
  if (motif->size) {
    if (motif->size != pos_i) {
      fprintf(stderr, "Error: Motif [%s] has rows with differing numbers of counts.",
        motif->name); return 1;
    }
  } else {
    motif->size = pos_i;
  }
  return 0;
}

void pcm_to_pwm(motif_t *motif) {
  int nsites = 0, nsites2;
  for (int i = 0; i < 4; i++) {
    nsites += get_score_i(motif, i, 0);
  }
  for (int j = 0; j < motif->size; j++) {
    nsites2 = 0;
    for (int i = 0; i < 4; i++) {
      nsites2 += get_score_i(motif, i, j);
    }
    if (abs(nsites2 - nsites) > 1) {
      fprintf(stderr, "Error: Column sums for motif [%s] are not equal.", motif->name);
      badexit("");
    } else if (abs(nsites2 - nsites) == 1 && args.w) {
      fprintf(stderr, "Warning: Found difference of 1 between column sums for motif [%s].",
        motif->name);
    }
  }
  char lets[] = { 'A', 'C', 'G', 'T' };
  for (int j = 0; j < motif->size; j++) {
    for (int i = 0; i < 4; i++) {
      set_score(motif, lets[i], j,
          calc_score((double) get_score_i(motif, i, j) / (double) nsites, args.bkg[i]));
    }
  }
}

void read_jaspar(void) {
  motif_info.fmt = FMT_JASPAR;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  int motif_i = -1, line_num = 0, row_i = -1;
  while ((read = getline(&line, &len, files.m)) != -1) {
    line_num++;
    if (line[0] == '>') {
      if (motif_i >= 0 && args.w) fprintf(stderr, "%d)\n", motifs[motif_i]->size);
      if (motif_i > -1 && row_i != 4) {
        if (row_i < 4) {
          if (args.w) fprintf(stderr, "\n");
          fprintf(stderr, "Error: Motif [%s] has too few rows", motifs[motif_i]->name);
        } else {
          if (args.w) fprintf(stderr, "\n");
          fprintf(stderr, "Error: Motif [%s] has too many rows", motifs[motif_i]->name);
        }
        badexit("");
      }
      motif_i++;
      if (add_motif()) {
        free(line);
        badexit("");
      }
      motifs[motif_i]->file_line_num = line_num;
      parse_jaspar_name(line, motif_i);
      row_i = 0;
    } else if (count_nonempty_chars(line)) {
      row_i++;
      if (add_jaspar_row(motifs[motif_i], line)) {
        free(line);
        badexit("");
      }
    }
  }
  free(line);
  if (motif_i > -1 && row_i != 4) {
    if (row_i < 4) {
      if (args.w) fprintf(stderr, "\n");
      fprintf(stderr, "Error: Motif [%s] has too few rows", motifs[motif_i]->name);
    } else {
      if (args.w) fprintf(stderr, "\n");
      fprintf(stderr, "Error: Motif [%s] has too many rows", motifs[motif_i]->name);
    }
    badexit("");
  }
  if (motif_i >= 0 && args.w) fprintf(stderr, "%d)\n", motifs[motif_i]->size);
  for (int i = 0; i < motif_info.n; i++) {
    pcm_to_pwm(motifs[i]);
  }
  if (args.v) {
    fprintf(stderr, "Found %'d JASPAR motif(s).\n", motif_info.n);
  }
}

void load_motifs() {
  switch (detect_motif_fmt()) {
    case FMT_MEME:
      read_meme();
      break;
    case FMT_HOMER:
      read_homer();
      break;
    case FMT_JASPAR:
      read_jaspar();
      break;
    case FMT_UNKNOWN:
      badexit("Error: Failed to detect motif format.");
      break;
  }
  complete_motifs();
  int empty_motifs = 0;
  for (int i = 0; i < motif_info.n; i++) if (!motifs[i]->size) empty_motifs++;
  if (empty_motifs == motif_info.n) {
    badexit("Error: All parsed motifs are empty.");
  } else if (empty_motifs) {
    fprintf(stderr, "Warning: Found %'d empty motifs.\n", empty_motifs);
  }
}

void count_bases(void) {
  for (int i = 0; i < seq_info.n; i++) {
    for (int j = 0; j < seq_sizes[i]; j++) {
      char_counts[seqs[i][j]]++;
    }
  }
}

void count_bases_single(const unsigned char *seq, const int len) {
  for (int i = 0; i < len; i++) char_counts[seq[i]]++;
}

static inline int standard_base_count() {
  return
    char_counts['A'] + char_counts['a'] +
    char_counts['C'] + char_counts['c'] +
    char_counts['G'] + char_counts['g'] +
    char_counts['U'] + char_counts['u'] +
    char_counts['T'] + char_counts['t'];
}

double calc_gc(void) {
  double gc = (double) (char_counts['G'] + char_counts['C'] +
      char_counts['g'] + char_counts['c']);
  gc /= standard_base_count();
  return gc;
}

void load_seqs() {
  int name_loaded = 0, line_len = 0, seq_i = -1, line_num = 0, line_len_no_spaces = 0;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  while ((read = getline(&line, &len, files.s)) != -1) {
    line_num++;
    if (!count_nonempty_chars(line)) continue;
    if (line[read - 2] == '\r') {
      line[read - 2] = '\0';
      line_len = read - 2;
    } else if (line[read - 1] == '\n') {
      line[read - 1] = '\0';
      line_len = read - 1;
    }
    if (!line_len) continue;
    if (line[0] == '>') {
      seq_i++;
      char **tmp_ptr1 = realloc(seq_names, sizeof(*seq_names) * (1 + seq_i));
      if (tmp_ptr1 == NULL) {
        free(line);
        badexit("Error: Failed to allocate memory for sequence names.");
      } else {
        seq_names = tmp_ptr1;
      }
      unsigned char **tmp_ptr2 = realloc(seqs, sizeof(*seqs) * (1 + seq_i));
      if (tmp_ptr2 == NULL) {
        free(line);
        badexit("Error: Failed to allocate memory for sequences.");
      } else {
        seqs = tmp_ptr2;
      }
      int *tmp_ptr3 = realloc(seq_sizes, sizeof(*seq_sizes) * (1 + seq_i));
      if (tmp_ptr3 == NULL) {
        free(line);
        badexit("Error: Failed to allocate memory for sequence sizes.");
      } else {
        seq_sizes = tmp_ptr3;
      }
      seq_sizes[seq_i] = 0;
      int *tmp_ptr5 = realloc(seq_line_nums, sizeof(*seq_line_nums) * (1 + seq_i));
      if (tmp_ptr5 == NULL) {
        free(line);
        badexit("Error: Failed to allocate memory for sequence line numbers.");
      } else {
        seq_line_nums = tmp_ptr5;
      }
      seq_line_nums[seq_i] = line_num;
      seq_names[seq_i] = malloc(sizeof(**seq_names) * SEQ_NAME_MAX_CHAR);
      if (seq_names[seq_i] == NULL) {
        free(line);
        badexit("Error: Failed to allocate memory for sequence names.");
      }
      ERASE_ARRAY(seq_names[seq_i], SEQ_NAME_MAX_CHAR);
      for (int i = 0; i < SEQ_NAME_MAX_CHAR; i++) {
        if (line[i + 1] == '\0') break;
        seq_names[seq_i][i] = line[i + 1];
      }
      name_loaded = 1;
      seq_sizes[seq_i] = 0;
    } else if (name_loaded) {
      line_len_no_spaces = 0;
      for (int i = 0; i < line_len; i++) if (line[i] != ' ') line_len_no_spaces++;
      if (seq_sizes[seq_i]) {
        unsigned char *tmp_ptr4 = realloc(seqs[seq_i],
          sizeof(**seqs) * (seq_sizes[seq_i] + line_len_no_spaces));
        if (tmp_ptr4 == NULL) {
          free(line);
          badexit("Error: Failed to allocate memory for sequences.");
        } else {
          seqs[seq_i] = tmp_ptr4;
        }
      } else {
        seqs[seq_i] = malloc(sizeof(**seqs) * line_len_no_spaces);
        if (seqs[seq_i] == NULL) {
          free(line);
          badexit("Error: Failed to allocate memory for sequences.");
        }
      }
      for (int j = 0, i = 0; i < line_len; i++) {
        if (line[i] != ' ') {
          seqs[seq_i][j + seq_sizes[seq_i]] = line[i]; j++;
        }
      }
      seq_sizes[seq_i] += line_len_no_spaces;
    }
  }
  free(line);
  if (seq_i == -1) {
    badexit("Error: Sequences don't appear to be fasta-formatted.");
  }
  seq_info.n = seq_i + 1;
  ERASE_ARRAY(char_counts, 256);
  count_bases();
  int seq_len_total = 0;
  for (int i = 0; i < seq_info.n; i++) seq_len_total += seq_sizes[i];
  if (!seq_len_total) {
    badexit("Error: Only encountered empty sequences.");
  }
  seq_info.total_bases = seq_len_total;
  seq_info.unknowns = seq_len_total - standard_base_count();
  seq_info.gc_pct = calc_gc() * 100.0;
  double unknowns_pct = 100.0 * seq_info.unknowns / seq_len_total;
  if (seq_info.unknowns == seq_len_total) {
    badexit("Error: Failed to read any standard DNA/RNA bases.");
  } else if (unknowns_pct >= 90.0) {
    fprintf(stderr, "!!! Warning: Non-standard base count is extremely high!!! (%.2f%%)\n",
      unknowns_pct);
  } else if (unknowns_pct >= 50.0 && args.v) {
    fprintf(stderr, "Warning: Non-standard base count is very high! (%.2f%%)\n",
      unknowns_pct);
  } else if (unknowns_pct >= 10.0 && args.v) {
    fprintf(stderr, "Warning: Non-standard base count seems high. (%.2f%%)\n",
      unknowns_pct);
  }
  if (char_counts[32] && args.v) {
    fprintf(stderr,
      "Internal warning: Found spaces (%'d) in loaded sequences, alert maintainer.\n",
      char_counts[32]);
  }
  if (args.v) {
    fprintf(stderr, "Loaded %'d sequence(s).\n    size=%'d    GC=%.2f%%\n",
      seq_info.n, seq_len_total, seq_info.gc_pct);
    if (seq_info.unknowns) {
      fprintf(stderr, "Found %'d (%.2f%%) non-standard bases.\n",
        seq_info.unknowns, unknowns_pct);
    }
    fprintf(stderr, "Approx. memory usage by sequence(s): %'.2f MB\n",
      b2mb(sizeof(unsigned char) * seq_len_total + sizeof(int) * seq_info.n +
        sizeof(char) * SEQ_NAME_MAX_CHAR * seq_info.n));
  }
}

int char_arrays_are_equal(const char *arr1, const char *arr2, const int len) {
  int are_equal = 1;
  for (int i = 0; i < len; i++) {
    if (arr1[i] == '\0' && arr2[i] == '\0') {
      break;
    }
    if (arr1[i] != arr2[i]) {
      are_equal = 0;
      break;
    }
  }
  return are_equal;
}

void int_to_char_array(const int L, const int N, char *arr) {
  ERASE_ARRAY(arr, 128);
  sprintf(arr, "__N%d_L%d", N, L);
}

int dedup_char_array(char *arr, const int arr_max_len, const int L, const int N) {
  int arr_len = 0, dedup_len = 0, success = 0, j = 0;
  char dedup[128];
  ERASE_ARRAY(dedup, 128);
  int_to_char_array(L, N, dedup);
  for (int i = 0; i < arr_max_len; i++) {
    if (arr[i] == '\0') {
      arr_len = i;
      break;
    }
  }
  for (int i = 0; i < 128; i++) {
    if (dedup[i] == '\0') {
      dedup_len = i + 1;
      break;
    }
  }
  if (arr_max_len - arr_len >= dedup_len) {
    for (int i = arr_len; i < arr_len + dedup_len; i++) {
      arr[i] = dedup[j];
      j++;
    }
    success = 1;
  }
  return success;
}

void find_motif_dupes() {
  if (motif_info.n == 1) return;
  int *is_dup = malloc(sizeof(int) * motif_info.n);
  if (is_dup == NULL) {
    badexit("Error: Failed to allocate memory for motif name duplication check.");
  }
  ERASE_ARRAY(is_dup, motif_info.n);
  for (int i = 0; i < motif_info.n - 1; i++) {
    for (int j = i + 1; j < motif_info.n; j++) {
      if (is_dup[j]) continue;
      if (char_arrays_are_equal(motifs[i]->name, motifs[j]->name, MAX_NAME_SIZE)) {
        is_dup[i] = 1; is_dup[j] = 1;
      }
    }
  }
  int dup_count = 0;
  for (int i = 0; i < motif_info.n; i++) dup_count += is_dup[i];
  if (dup_count) {
    if (args.dedup) {
      for (int i = 0; i < motif_info.n; i++) {
        if (is_dup[i]) {
          int success = dedup_char_array(motifs[i]->name, MAX_NAME_SIZE,
            motifs[i]->file_line_num, i + 1);
          if (!success) {
            fprintf(stderr,
              "Error: Failed to deduplicate motif #%d, name is too large.", i + 1);
            free(is_dup);
            badexit("");
          }
        }
      }
    } else {
      fprintf(stderr,
        "Error: Encountered duplicate motif name (use -d to deduplicate).");
      int to_print = 5;
      if (to_print > dup_count) to_print = dup_count;
      for (int i = 0; i < motif_info.n; i++) {
        if (is_dup[i]) {
          fprintf(stderr, "\n    L%d #%d: %s", motifs[i]->file_line_num, i + 1,
            motifs[i]->name);
          to_print--;
          if (!to_print) break;
        }
      }
      if (dup_count > 5) {
        fprintf(stderr, "\n    ...");
        fprintf(stderr, "\n    Found %'d total non-unique names.", dup_count);
      }
      free(is_dup);
      badexit("");
    }
  }
}

void find_seq_dupes() {
  if (seq_info.n == 1) return;
  int *is_dup = malloc(sizeof(int) * seq_info.n);
  ERASE_ARRAY(is_dup, seq_info.n);
  for (int i = 0; i < seq_info.n - 1; i++) {
    for (int j = i + 1; j < seq_info.n; j++) {
      if (is_dup[j]) continue;
      if (char_arrays_are_equal(seq_names[i], seq_names[j], SEQ_NAME_MAX_CHAR)) {
        is_dup[i] = 1; is_dup[j] = 1;
      }
    }
  }
  int dup_count = 0;
  for (int i = 0; i < seq_info.n; i++) dup_count += is_dup[i];
  if (dup_count) {
    if (args.dedup) {
      for (int i = 0; i < seq_info.n; i++) {
        if (is_dup[i]) {
          int success = dedup_char_array(seq_names[i], SEQ_NAME_MAX_CHAR,
            seq_line_nums[i], i + 1);
          if (!success) {
            fprintf(stderr,
              "Error: Failed to deduplicate sequence #%d, name is too large.", i + 1);
            free(is_dup);
            badexit("");
          }
        }
      }
    } else {
      fprintf(stderr,
        "Error: Encountered duplicate sequence name (use -d to deduplicate).");
      int to_print = 5;
      if (to_print > dup_count) to_print = dup_count;
      for (int i = 0; i < seq_info.n; i++) {
        if (is_dup[i]) {
          fprintf(stderr, "\n    L%d #%d: %s", seq_line_nums[i], i + 1, seq_names[i]);
          to_print--;
          if (!to_print) break;
        }
      }
      if (dup_count > 5) {
        fprintf(stderr, "\n    ...");
        fprintf(stderr, "\n    Found %'d total non-unique names.", dup_count);
      }
      free(is_dup);
      badexit("");
    }
  }
  free(is_dup);
}

static inline void score_subseq(const motif_t *motif, const unsigned char *seq, const int offset, int *score) {
  *score = 0;
  for (int i = 0; i < motif->size; i++) {
    *score += get_score(motif, seq[i + offset], i);
  }
}

static inline void score_subseq_rc(const motif_t *motif, const unsigned char *seq, const int offset, int *score) {
  *score = 0;
  for (int i = 0; i < motif->size; i++) {
    *score += get_score_rc(motif, seq[i + offset], i);
  }
}

void score_seq(const int motif_i, const int seq_i) {
  if (seq_sizes[seq_i] < motifs[motif_i]->size ||
      motifs[motif_i]->threshold == INT_MAX) {
    return;
  }
  int score = INT_MIN;
  for (int i = 0; i <= seq_sizes[seq_i] - motifs[motif_i]->size; i++) {
    score_subseq(motifs[motif_i], seqs[seq_i], i, &score); 
    if (score >= motifs[motif_i]->threshold) {
      fprintf(files.o, "%s\t%i\t%i\t+\t%s\t%.9g\t%.3f\t%.1f\t%.*s\n",
        seq_names[seq_i],
        i + 1,
        i + motifs[motif_i]->size,
        motifs[motif_i]->name,
        score2pval(motifs[motif_i], score),
        score / PWM_INT_MULTIPLIER,
        100.0 * score / motifs[motif_i]->max_score,
        motifs[motif_i]->size,
        seqs[seq_i] + i);
    }
  }
  if (args.scan_rc) {
    for (int i = 0; i <= seq_sizes[seq_i] - motifs[motif_i]->size; i++) {
      score_subseq_rc(motifs[motif_i], seqs[seq_i], i, &score); 
      if (score >= motifs[motif_i]->threshold) {
        fprintf(files.o, "%s\t%i\t%i\t-\t%s\t%.9g\t%.3f\t%.1f\t%.*s\n",
          seq_names[seq_i],
          i + 1,
          i + motifs[motif_i]->size,
          motifs[motif_i]->name,
          score2pval(motifs[motif_i], score),
          score / PWM_INT_MULTIPLIER,
          100.0 * score / motifs[motif_i]->max_score,
          motifs[motif_i]->size,
          seqs[seq_i] + i);
      }
    }
  }
}

void print_seq_stats(FILE *whereto) {
  for (int i = 0; i < seq_info.n; i++) {
    ERASE_ARRAY(char_counts, 256);
    count_bases_single(seqs[i], seq_sizes[i]);
    fprintf(whereto, "%d\t%d\t%s\t", i + 1, seq_line_nums[i], seq_names[i]);
    fprintf(whereto, "%d\t", seq_sizes[i]);
    if (!seq_sizes[i]) {
      fprintf(whereto, "nan\t");
    } else {
      fprintf(whereto, "%.2f\t", calc_gc() * 100.0);
    }
    fprintf(whereto, "%d\n", seq_sizes[i] - standard_base_count());
  }
}

void trim_seq_names(void) {
  for (int i = 0; i < seq_info.n; i++) {
    for (int j = 0; j < SEQ_NAME_MAX_CHAR; j++) {
      if (seq_names[i][j] == ' ') {
        seq_names[i][j] = '\0';
      }
      if (seq_names[i][j] == '\0') break;
    }
  }
}

void add_consensus_motif(const char *consensus) {
  if (add_motif()) badexit("");
  ERASE_ARRAY(motifs[0]->name, MAX_NAME_SIZE);
  int i = 0;
  for (;;) {
    motifs[0]->name[i] = consensus[i];
    if (consensus[i] == '\0') {
      motifs[0]->size = i;
      break;
    }
    i++;
  }
  if (motifs[0]->size > MAX_MOTIF_SIZE / 5) {
    fprintf(stderr, "Error: Consensus sequence is too large (%d>max=%d).",
      motifs[0]->size, MAX_MOTIF_SIZE / 5);
  }
  int let_i;
  for (int pos = 0; pos < motifs[0]->size; pos++) {
    let_i = consensus2index[(unsigned char) consensus[pos]];
    if (let_i < 0) {
      fprintf(stderr, "Error: Encountered unknown letter in consensus (%c).",
        consensus[pos]);
      badexit("");
    }
    set_score(motifs[0], 'A', pos,
      calc_score(consensus2probs[let_i * 4 + 0], args.bkg[0]));
    set_score(motifs[0], 'C', pos,
      calc_score(consensus2probs[let_i * 4 + 1], args.bkg[1]));
    set_score(motifs[0], 'G', pos,
      calc_score(consensus2probs[let_i * 4 + 2], args.bkg[2]));
    set_score(motifs[0], 'T', pos,
      calc_score(consensus2probs[let_i * 4 + 3], args.bkg[3]));
  }
  complete_motifs();
}

void print_pb(const double prog) {
  int left = prog * PROGRESS_BAR_WIDTH;
  int right = PROGRESS_BAR_WIDTH - left;
  fprintf(stderr, "\r[%.*s%*s] %3d%%", left, PROGRESS_BAR_STRING, right, "",
      (int) (prog * 100.0));
  fflush(stderr);
}

int main(int argc, char **argv) {

  if (setlocale(LC_NUMERIC, "en_US") == NULL && args.v) {
    fprintf(stderr, "Warning: setlocale(LC_NUMERIC, \"en_US\") failed.\n");
  }

  motifs = malloc(sizeof(*motifs));
  if (motifs == NULL) {
    badexit("Error: Failed to allocate memory for motifs.");
  }
  seq_names = malloc(sizeof(*seq_names));
  if (seq_names == NULL) {
    badexit("Error: Failed to allocate memory for sequence names.");
  }
  seqs = malloc(sizeof(*seqs));
  if (seqs == NULL) {
    badexit("Error: Failed to allocate memory for sequences.");
  }
  seq_sizes = malloc(sizeof(*seq_sizes));
  if (seq_sizes == NULL) {
    badexit("Error: Failed to allocate memory for sequence sizes.");
  }
  seq_line_nums = malloc(sizeof(*seq_line_nums));
  if (seq_line_nums == NULL) {
    badexit("Error: Failed to allocate memory for sequence line numbers.");
  }

  char *user_bkg, *consensus;
  int use_stdout = 1, has_motifs = 0, has_seqs = 0, has_consensus = 0;

  int opt;

  while ((opt = getopt(argc, argv, "m:1:s:o:b:flt:p:n:gdrvwh")) != -1) {
    switch (opt) {
      case 'm':
        if (has_consensus) {
          badexit("Error: -m and -1 cannot both be used.");
        }
        has_motifs = 1;
        files.m = fopen(optarg, "r");
        if (files.m == NULL) {
          fprintf(stderr, "Error: Failed to open motif file: %s", optarg);
          badexit("");
        }
        files.m_open = 1;
        break;
      case '1':
        if (has_motifs) {
          badexit("Error: -m and -1 cannot both be used.");
        }
        has_consensus = 1;
        consensus = optarg;
        break;
      case 's':
        has_seqs = 1;
        if (optarg[0] == '-' && optarg[1] == '\0') {
          files.s = stdin;
        } else {
          files.s = fopen(optarg, "r");
          if (files.s == NULL) {
            fprintf(stderr, "Error: Failed to open sequence file: %s", optarg);
            badexit("");
          }
        }
        files.s_open = 1;
        break;
      case 'o':
        use_stdout = 0;
        files.o = fopen(optarg, "w");
        if (files.o == NULL) {
          fprintf(stderr, "Error: Failed to create output file: %s", optarg);
          badexit("");
        }
        files.o_open = 1;
        break;
      case 'b':
        args.use_user_bkg = 1;
        user_bkg = optarg;
        break;
      case 'f':
        args.scan_rc = 0;
        break;
      case 't':
        args.pvalue = atof(optarg);
        break;
      case 'p':
        args.pseudocount = atof(optarg);
        if (!args.pseudocount) {
          badexit("Error: -p must be a positive integer.");
        }
        break;
      case 'n':
        args.nsites = atoi(optarg);
        if (!args.nsites) {
          badexit("Error: -n must be a positive integer.");
        }
        break;
      case 'd':
        args.dedup = 1;
        break;
      case 'r':
        args.trim_names = 1;
        break;
      case 'g':
        args.progress = 1;
        break;
      case 'w':
        args.w = 1;
      case 'v':
        args.v = 1;
        break;
      case 'h':
        usage();
        return EXIT_SUCCESS;
      default:
        return EXIT_FAILURE;
    }
  }

  if (use_stdout) {
    files.o = stdout;
    files.o_open = 1;
  }

  if (!has_seqs && !has_motifs && !has_consensus) {
    badexit("Error: Missing one of -m, -1, -s args.");
  }

  if (args.use_user_bkg) parse_user_bkg(user_bkg);

  if (has_consensus) {
    args.bkg[0] = 0.25; args.bkg[1] = 0.25; args.bkg[2] = 0.25; args.bkg[3] = 0.25;
    args.pvalue = 1;
    args.nsites = 1000;
    args.pseudocount = 1;
    add_consensus_motif(consensus);
    has_motifs = 1;
  }

  if (has_motifs) {
    if (!has_consensus) {
      load_motifs();
      find_motif_dupes();
    }
    if (!has_seqs) {
      if (args.v) {
        fprintf(stderr, "No sequences provided, parsing + printing motifs before exit.\n");
      }
      for (int i = 0; i < motif_info.n; i++) {
        fill_cdf(motifs[i]);
        set_threshold(motifs[i]);
        if (has_consensus) motifs[0]->threshold = motifs[0]->max_score;
        fprintf(files.o, "----------------------------------------\n");
        print_motif(motifs[i], i + 1);
        free(motifs[i]->cdf);
        motifs[i]->cdf_size = 0;
      }
      fprintf(files.o, "----------------------------------------\n");
    }
  }
  if (has_seqs) {
    time_t time1 = time(NULL);
    if (args.v) {
      fprintf(stderr, "Reading sequences ...\n");
    }
    load_seqs();
    if (args.trim_names) trim_seq_names();
    find_seq_dupes();
    time_t time2 = time(NULL);
    if (args.v) {
      time_t time3 = difftime(time2, time1);
      if (time3 > 1) {
        fprintf(stderr, "Needed %'d seconds to load sequences.\n",
          (int) time3);
      }
    }
    if (!has_motifs) {
      if (args.v) {
        fprintf(stderr, "No motifs provided, printing sequence stats before exit.\n");
      }
      fprintf(files.o, "##seqnum\tline_num\tseqname\tsize\tgc_pct\tn_count\n");
      print_seq_stats(files.o);
    }
  }

  if (has_seqs && has_motifs) {

    fprintf(files.o, "##minimotif v%s [ ", MINIMOTIF_VERSION);
    for (int i = 1; i < argc; i++) {
      fprintf(files.o, "%s ", argv[i]);
    }
    fprintf(files.o, "]\n");
    int motif_size = 0;
    for (int i = 0; i < motif_info.n; i++) motif_size += motifs[i]->size;
    fprintf(files.o,
        "##MotifCount=%d MotifSize=%d SeqCount=%d SeqSize=%d GC=%.2f%% Ns=%d\n",
        motif_info.n, motif_size, seq_info.n, seq_info.total_bases, seq_info.gc_pct,
        seq_info.unknowns);
    fprintf(files.o, 
      "##seqname\tstart\tend\tstrand\tmotif\tpvalue\tscore\tscore_pct\tmatch\n");

    if (args.v) fprintf(stderr, "Scanning ...\n");
    time_t time1 = time(NULL);
    for (int i = 0; i < motif_info.n; i++) {
      if (args.progress) {
        print_pb((i + 1.0) / motif_info.n);
      } else if (args.w) {
        fprintf(stderr, "    Scanning motif: %s\n", motifs[i]->name);
      }
      fill_cdf(motifs[i]);
      set_threshold(motifs[i]);
      if (has_consensus) motifs[0]->threshold = motifs[0]->max_score;
      for (int j = 0; j < seq_info.n; j++) {
        if (!args.progress && args.w) {
          fprintf(stderr, "        Scanning sequence: %s\n", seq_names[j]);
        }
        score_seq(i, j);
      }
      free(motifs[i]->cdf);
      motifs[i]->cdf_size = 0;
    }
    if (args.progress) fprintf(stderr, "\n");
    time_t time2 = time(NULL);
    time_t time3 = difftime(time2, time1);
    if (args.v) fprintf(stderr, "Done.\n");
    if (args.v && time3 > 1) fprintf(stderr, "Needed %'d seconds to scan.\n",
        (int) time3);

  }

  close_files();
  free_motifs();
  free_seqs();

  return EXIT_SUCCESS;

}

