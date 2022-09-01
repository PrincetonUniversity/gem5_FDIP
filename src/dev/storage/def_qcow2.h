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

#ifndef _DEF_QCOW2_H
#define _DEF_QCOW2_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> // fopen() etc
#include <asm/byteorder.h> // __be32_to_cpu() __be64_to_cpu() for big endian to CPU endian

#include "qcow2.h"
#include "qcow2_header.h"
#include "qcow2_state.h"

#define QCOW2_INCOMPAT_EXTL2 1 << 4 // If this feature bit is set the L2 entry size changes

/* Size of normal and extended L2 entries */
#define L2E_SIZE_NORMAL   (sizeof(uint64_t))
#define L2E_SIZE_EXTENDED (sizeof(uint64_t) * 2)

#define L1_OFFSET_MASK 0x00ffffffffffff00ULL // L1 Offset only use bits 9-55
#define L2_OFFSET_MASK 0x3fffffffffffffffULL // L2 Offset use bits 0-61

static inline int64_t offset_into_cluster(qcow2_state *c, uint64_t offset) {
   return offset & (c->cluster_size -1);
}

static inline int has_subclusters(qcow2_state *c) {
    return c->_header.incompatible_features & QCOW2_INCOMPAT_EXTL2;
}

static inline size_t l2_entry_size(qcow2_state *c) {
    return has_subclusters(c) ? L2E_SIZE_EXTENDED : L2E_SIZE_NORMAL;
}

static inline int offset_to_l1_index(qcow2_state *c, uint64_t offset) {
  return offset >> (c->l2_bits + c->_header.cluster_bits);
}

static inline int offset_to_l2_index(qcow2_state *c, int64_t offset) {
    return (offset >> c->_header.cluster_bits) & (c->l2_size - 1);
}

#endif
