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
#ifndef _QCOW2_STATE_H_
#define _QCOW2_STATE_H_

#include "qcow2_header.h"

/* Keeps track of the state of the currently opened QCow2 file */

typedef struct qcow2_state {
  FILE *fp;
  QCow2Header _header;

  char *currentFile;
  char *backingFile;

  int cluster_size; // Size of each cluster

  uint64_t *l1_table;

  int l2_bits;        // Number of bits needed for the L2 Index
  int l2_size;        // Number of L2 entries

} qcow2_state;


#endif
