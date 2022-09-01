/*
    read_qcow2
    Copyright (C) 2020  Peter Hansen (phaanx@gmail.com)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "qcow2.h"
#include "def_qcow2.h"
#include "qcow2_state.h"

typedef struct {
  uint64_t l2_offset;
  uint64_t *l2_table;

} l2_cache_t;

const uint32_t L2_CACHE_SIZE = 2048*1024; // Allocate 2 Mb for l2 cache

uint64_t *l2_cache[2048*1024];

uint64_t *qcow2_l2_load(qcow2_state *c, uint64_t l2_offset) {
  //printf("Loading L2 table at offset: %"PRIu64"\n", l2_offset);
  uint32_t cluster = l2_offset / c->cluster_size;
  //printf("cluster: %"PRIu64"\n", cluster);

  if( cluster >= L2_CACHE_SIZE) {
    printf("Cluster %i > l2 cache size %i\n", cluster, L2_CACHE_SIZE);
    return NULL;
  }

  uint64_t *buf;
  if( l2_cache[cluster] == 0) {
    printf("Loading L2 cluster %i from file\n", cluster);

    u_int32_t buf_size = c->cluster_size;
    buf = malloc(buf_size);

    if( buf == NULL) {
      printf("Failed to allocate L2 table.\n");
      return NULL;
    }

    if( fseek(c->fp, l2_offset, SEEK_SET) != 0 ) {
      printf("Failed to read l2 table (fseek)\n");
      return NULL;
    }
    if( fread(buf, buf_size, 1, c->fp) != 1 ) {
      printf("Failed to read l2 table (fread)\n");
      return NULL;
    }

    for(int i=0; i<c->l2_size; i++) {
      buf[i] = __be64_to_cpu(buf[i]);
    }
    l2_cache[cluster] = buf;

  } else {
    //printf("Loading l2 from cache\n");
    buf = l2_cache[cluster];
  }

  return buf;
}

void qcow2_l2_free() {
  for(int i=0; i<1024; i++) {
    if( l2_cache[i] != 0) {
      free(l2_cache[i]);
    }
  }
}

void qcow2_l2_dump(uint64_t *l2buf, int num) {
  for(int i=0; i<num; i++) {
    printf("I: %"PRIu64"\n", l2buf[i]);
  }
}