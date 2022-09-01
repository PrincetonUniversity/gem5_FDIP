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

int qcow2_l1_load(qcow2_state *c) {

  u_int32_t l1_size = c->_header.l1_size * sizeof(uint64_t);
  c->l1_table = malloc(l1_size);

  printf("Loading L1 table at offset: %"PRIu64"\n", c->_header.l1_table_offset);

  if( fseek(c->fp, c->_header.l1_table_offset, SEEK_SET) != 0 ) {
      printf("Failed to read l1 table (fseek)\n");
      return 1;
  }
  if( fread(c->l1_table, l1_size, 1, c->fp) != 1 ) {
    printf("Failed to read l1 table (fread)\n");
    return 1;
  }

  /* Convert Big endian to the endian used by the CPU */
  for(int i=0; i<c->_header.l1_size; i++) {
    c->l1_table[i] = __be64_to_cpu(c->l1_table[i]);
  }

  return 0;
}

void qcow2_l1_dump(qcow2_state *c) {
  for(int i=0; i<c->_header.l1_size; i++) {
    printf("I: %"PRIu64"\n", c->l1_table[i]);
  }
}