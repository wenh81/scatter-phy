/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2015 Software Radio Systems Limited
 *
 * \section LICENSE
 *
 * This file is part of the srsLTE library.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "srslte/sync/sss.h"
#include "srslte/dft/dft.h"
#include "srslte/utils/convolution.h"
#include "srslte/utils/vector.h"

void generate_sss_all_tables(srslte_sss_tables_t *tables, uint32_t N_id_2);
void convert_tables(srslte_sss_fc_tables_t *fc_tables, srslte_sss_tables_t *in);
void generate_N_id_1_table(uint32_t table[30][30]);
// ******************* Scatter customized implementations *******************
void convert_sch_tables(srslte_sch_fc_tables_t *sch_fc_tables, srslte_sch_tables_t *in);
void generate_sch_all_tables(srslte_sch_tables_t *tables, uint32_t sch_table_index);
void generate_sch_table(uint32_t table[SRSLTE_SCH_N][SRSLTE_SCH_N]);
// ******************* Scatter customized implementations *******************

int srslte_sss_synch_init(srslte_sss_synch_t *q, uint32_t fft_size) {

  if (q                 != NULL  &&
      fft_size          <= 2048)
  {
    uint32_t N_id_2;
    srslte_sss_tables_t sss_tables;

    bzero(q, sizeof(srslte_sss_synch_t));

    if (srslte_dft_plan(&q->dftp_input, fft_size, SRSLTE_DFT_FORWARD, SRSLTE_DFT_COMPLEX)) {
      srslte_sss_synch_free(q);
      return SRSLTE_ERROR;
    }
    srslte_dft_plan_set_mirror(&q->dftp_input, true);
    srslte_dft_plan_set_dc(&q->dftp_input, true);

    q->fft_size = fft_size;

    generate_N_id_1_table(q->N_id_1_table);

    for (N_id_2=0;N_id_2<3;N_id_2++) {
      generate_sss_all_tables(&sss_tables, N_id_2);
      convert_tables(&q->fc_tables[N_id_2], &sss_tables);
    }
    q->N_id_2 = 0;

    // ---------------------------------------------
    // Scatter implementation.
    // Generate Scatter table.
    q->sch_table_index = 0; // By default we always use index 0.
    generate_sch_table(q->sch_table);
    srslte_sch_tables_t sch_tables;
    generate_sch_all_tables(&sch_tables, q->sch_table_index);
    convert_sch_tables(&q->sch_fc_tables[q->sch_table_index], &sch_tables);
    // ---------------------------------------------

    return SRSLTE_SUCCESS;
  }
  return SRSLTE_ERROR_INVALID_INPUTS;
}

int srslte_sss_synch_realloc(srslte_sss_synch_t *q, uint32_t fft_size) {
  if (q                 != NULL  &&
      fft_size          <= 2048)
  {
    srslte_dft_plan_free(&q->dftp_input);
    if (srslte_dft_plan(&q->dftp_input, fft_size, SRSLTE_DFT_FORWARD, SRSLTE_DFT_COMPLEX)) {
      srslte_sss_synch_free(q);
      return SRSLTE_ERROR;
    }
    srslte_dft_plan_set_mirror(&q->dftp_input, true);
    srslte_dft_plan_set_norm(&q->dftp_input, true);
    srslte_dft_plan_set_dc(&q->dftp_input, true);

    q->fft_size = fft_size;
    return SRSLTE_SUCCESS;
  }
  return SRSLTE_ERROR_INVALID_INPUTS;
}

void srslte_sss_synch_free(srslte_sss_synch_t *q) {
  srslte_dft_plan_free(&q->dftp_input);
  bzero(q, sizeof(srslte_sss_synch_t));
}

/** Sets the N_id_2 to search for */
int srslte_sss_synch_set_N_id_2(srslte_sss_synch_t *q, uint32_t N_id_2) {
  if (!srslte_N_id_2_isvalid(N_id_2)) {
    fprintf(stderr, "Invalid N_id_2 %d\n", N_id_2);
    return SRSLTE_ERROR;
  } else {
    q->N_id_2 = N_id_2;
    return SRSLTE_SUCCESS;
  }
}

/** 36.211 10.3 section 6.11.2.2
 */
void srslte_sss_put_slot(float *sss, cf_t *slot, uint32_t nof_prb, srslte_cp_t cp) {
  uint32_t i, k;

  k = (SRSLTE_CP_NSYMB(cp) - 2) * nof_prb * SRSLTE_NRE + nof_prb * SRSLTE_NRE / 2 - 31;

  if (k > 5) {
    memset(&slot[k - 5], 0, 5 * sizeof(cf_t));
    for (i = 0; i < SRSLTE_SSS_LEN; i++) {
      __real__ slot[k + i] = sss[i];
      __imag__ slot[k + i] = 0;
    }
    memset(&slot[k + SRSLTE_SSS_LEN], 0, 5 * sizeof(cf_t));
  }
}

/** Sets the SSS correlation peak detection threshold */
void srslte_sss_synch_set_threshold(srslte_sss_synch_t *q, float threshold) {
  q->corr_peak_threshold = threshold;
}

/** Returns the subframe index based on the m0 and m1 values */
uint32_t srslte_sss_synch_subframe(uint32_t m0, uint32_t m1) {
  //printf("srslte_sss_synch_subframe m0 %d m1 %d\n", m0, m1);
  if (m1 > m0) {
    return 0;
  } else {
    return 5;
  }
}

/** Returns the N_id_1 value based on the m0 and m1 values */
int srslte_sss_synch_N_id_1(srslte_sss_synch_t *q, uint32_t m0, uint32_t m1) {
  int N_id_1 = -1;
  if (m1 > m0) {
    if (m0 < 30 && m1 - 1 < 30) {
      N_id_1 = q->N_id_1_table[m0][m1 - 1];
    }
  } else {
    if (m1 < 30 && m0 - 1 < 30) {
      N_id_1 = q->N_id_1_table[m1][m0 - 1];
    }
  }
  return N_id_1;
}

//******************************************************************************
//********************* Scatter signal related functions ***********************
//******************************************************************************
// Returns the SCH value based on the m0 and m1 values.
int srslte_sch_synch_value(srslte_sss_synch_t *q, uint32_t m0, uint32_t m1) {
  int sch_value = -1;
  if(m1 < SRSLTE_SCH_N && m0 < SRSLTE_SCH_N) {
    sch_value = q->sch_table[m1][m0];
  }
  return sch_value;
}

void srslte_sch_put_slot_generic(float *sch, cf_t *slot, uint32_t nof_prb, srslte_cp_t cp, uint32_t offset) {
  uint32_t i, k;

  k = (SRSLTE_CP_NSYMB(cp) - offset) * nof_prb * SRSLTE_NRE + nof_prb * SRSLTE_NRE / 2 - SRSLTE_SCH_N;

  if(k > 5) {
    memset(&slot[k - 5], 0, 5 * sizeof(cf_t));
    for(i = 0; i < SRSLTE_SCH_LEN; i++) {
      __real__ slot[k + i] = sch[i];
      __imag__ slot[k + i] = 0;
    }
    memset(&slot[k + SRSLTE_SCH_LEN], 0, 5 * sizeof(cf_t));
  }
}

void srslte_sss_put_slot_generic(float *sss, cf_t *slot, uint32_t nof_prb, srslte_cp_t cp, uint32_t offset) {
  uint32_t i, k;

  k = (SRSLTE_CP_NSYMB(cp) - offset) * nof_prb * SRSLTE_NRE + nof_prb * SRSLTE_NRE / 2 - SRSLTE_SSS_N;

  if(k > 5) {
    memset(&slot[k - 5], 0, 5 * sizeof(cf_t));
    for(i = 0; i < SRSLTE_SSS_LEN; i++) {
      __real__ slot[k + i] = sss[i];
      __imag__ slot[k + i] = 0;
    }
    memset(&slot[k + SRSLTE_SSS_LEN], 0, 5 * sizeof(cf_t));
  }
}

void srslte_sch_set_table_index(srslte_sss_synch_t *q, uint32_t index) {
  if(index >= 3) {
    fprintf(stderr, "Invalid table index: %d.\n", index);
  } else {
    q->sch_table_index = index;
  }
}

// Add SCH signal to the front of the resource grid/subframe.
// For 6 PRBs it adds to the symbol number # 1, which has no RS symbols in it, therefore, the SCH symbols are consecutively mapped to the grid.
// For 15 and 25 PRBs it adds to the symbol number # 0, which has RS symbols in it, and therefore, the SCH symbols are intercalated with RS symbols.
void srslte_sch_put_slot_front(float *sch, cf_t *slot, uint32_t nof_prb, srslte_cp_t cp) {
  int k;
  if(nof_prb != 6) {
    // Calculate initial location in symbol #0.
    k = nof_prb * SRSLTE_NRE / 2 - SRSLTE_SCH_N - (SRSLTE_SCH_N/2 + 1);
    for(uint32_t i = 0; i < SRSLTE_SCH_LEN; i += 2) {
      for(uint32_t j = 0; j < 2; j++) {
        __real__ slot[k + j] = sch[i + j];
        __imag__ slot[k + j] = 0;
      }
      k += 3;
    }
  } else {
    // Calculate initial location in symbol #1.
    k = nof_prb * SRSLTE_NRE + nof_prb * SRSLTE_NRE / 2 - SRSLTE_SCH_N;
    for(uint32_t i = 0; i < SRSLTE_SCH_LEN; i++) {
      __real__ slot[k + i] = sch[i];
      __imag__ slot[k + i] = 0;
    }
  }
}
