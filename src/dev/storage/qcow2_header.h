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
#ifndef _QCOW2_HEADER_H
#define _QCOW2_HEADER_H

#include <inttypes.h> // uint64_t & PRIu64 for printf

#define QCOW2_MAGIC (('Q' << 24) | ('F' << 16) | ('I' << 8) | 0xfb)

typedef struct QCow2Header {
  uint32_t magic;
  uint32_t version;

  uint64_t backing_file_offset;
  uint32_t backing_file_size;

  uint32_t cluster_bits;            // Number of bits for the in-cluster-offset part
  uint64_t size;                    // file size in bytes
  uint32_t crypt_method;

  uint32_t l1_size;
  uint64_t l1_table_offset;

  uint64_t refcount_table_offset;
  uint32_t refcount_table_clusters;

  uint32_t nb_snapshots;
  uint64_t snapshots_offset;

  /* Version >= 3 extended header */
  uint64_t incompatible_features;
  uint64_t compatible_features;
  uint64_t autoclear_features;

  uint32_t refcount_order;
  uint32_t header_length;

  uint8_t compression_type;

  /* padding to make header multiple of 8 */
  uint8_t padding[7];
} QCow2Header;

#endif