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
#include "qcow2_l1_table.h"
#include "qcow2_l2_table.h"

int qcow2_readHeader();
void qcow2_printInfo();

qcow2_state c; // State of current file

int read_backingFile() {
  uint64_t offset = c._header.backing_file_offset;
  uint32_t size = c._header.backing_file_size;

  printf("cluster size is %d\n",c.cluster_size);
  if( offset > c.cluster_size ) {
    printf("Backing file offset to large offset: %lu\n", offset);
    return 0;
  }

  if( fseek(c.fp, offset, SEEK_SET) != 0 ) {
    return 0;
  }
  c.backingFile =  (char*)malloc(size+2);
  c.backingFile[size+1] = '\0';
  return fread(c.backingFile, size, 1, c.fp);
}

int qcow2_openFile(const char *filename) {
  if( c.fp != NULL ) {
    qcow2_closeFile();
  }
  c.fp = fopen(filename, "rb");
  if( c.fp == NULL ) {
    printf("Error opening file.\n");
    return 1;
  }
  printf("QCow2 File %s opened.\n", filename);

  qcow2_readHeader();

  // Calculate cluster size
  c.cluster_size = 1 << c._header.cluster_bits;


  if( c._header.magic != QCOW2_MAGIC ) {
    printf("Error loading file, bad header.\n");
    return 1;
  }
  if( c._header.version < 2 || c._header.version > 3 ) {
    printf("Unsupported QCow2 version %" PRIu32 "\n", c._header.version);
    return 1;
  }

  printf("QCow2 file header verified.\n");

  read_backingFile();

  // Load L1 table
  qcow2_l1_load(&c);

  // L2 calculate number of entries in one L2 cluster 
  // and number of bits needed to store an index for an entry
  //c.l2_bits = c._header.cluster_bits - __builtin_ctz(l2_entry_size(&c));
  //c.l2_size = 1 << c.l2_bits;
  c.l2_size = c.cluster_size / l2_entry_size(&c);
  c.l2_bits = __builtin_ctz(c.l2_size);

  qcow2_printInfo(&c);

  return 0;
}

void qcow2_closeFile() {
  qcow2_l2_free();
  printf("Closing QCow2 file.\n");
  if( c.fp != NULL ) {
    fclose(c.fp);
  }
}

int qcow2_readHeader() {
  fread(&c._header, sizeof(c._header), 1, c.fp);

  // convert from big endian to CPU endian
  c._header.magic                   = __be32_to_cpu(c._header.magic);
  c._header.version                 = __be32_to_cpu(c._header.version);
  c._header.backing_file_offset     = __be64_to_cpu(c._header.backing_file_offset);
  c._header.backing_file_size       = __be32_to_cpu(c._header.backing_file_size);
  c._header.cluster_bits            = __be32_to_cpu(c._header.cluster_bits);
  c._header.size                    = __be64_to_cpu(c._header.size);
  c._header.crypt_method            = __be32_to_cpu(c._header.crypt_method);
  c._header.l1_size                 = __be32_to_cpu(c._header.l1_size);
  c._header.l1_table_offset         = __be64_to_cpu(c._header.l1_table_offset);
  c._header.refcount_table_offset   = __be64_to_cpu(c._header.refcount_table_offset);
  c._header.refcount_table_clusters = __be32_to_cpu(c._header.refcount_table_clusters);
  c._header.nb_snapshots            = __be32_to_cpu(c._header.nb_snapshots);
  c._header.snapshots_offset        = __be64_to_cpu(c._header.snapshots_offset);

  return 0;
}

uint64_t qcow2_getFileSize() {
  return c._header.size;
}

void qcow2_printInfo(qcow2_state *c) {
  // Print header
  printf("-------(QCow2 Header & details)---------\n");
  printf("Version: %i\n", c->_header.version);
  printf("Backing file: %s\n", c->backingFile);

  uint64_t file_size = c->_header.size;
  char suffix[5] = " kMGT";
  int si = 0;
  while(file_size > 1024) {
    file_size = file_size / 1024;
    si++;
  }
  printf("File size: %" PRIu64 "bytes", c->_header.size);
  printf(" (%" PRIu64 " %cb)\n", file_size, suffix[si]);
  printf("Cluster bits: %i\n", c->_header.cluster_bits);
  printf("Cluster size: %i\n", c->cluster_size);
  printf("Crypt method: %i\n", c->_header.crypt_method);
  printf("L1 offset: %" PRIu64 "\n", c->_header.l1_table_offset);
  printf("L1 size: %i\n", c->_header.l1_size);
  printf("L2 bits: %i\n", c->l2_bits);
  printf("L2 entries: %i\n", c->l2_size);
  printf("L2 entry size: %i\n", (int)l2_entry_size(c));
  printf("Refcount offset: %" PRIu64 "\n", c->_header.refcount_table_offset);
  printf("Refcount clusters: %i\n", c->_header.refcount_table_clusters);
  printf("Snapshots: %i\n", c->_header.nb_snapshots);
  printf("Snapshots offset: %" PRIu64 "\n", c->_header.snapshots_offset);
  printf("-------------------------------\n");
}

/**
 * @c: State of currently open QCow2 file
 * @offset: Location where to start to read
 * @bytes: Number of bytes to read, but without crossing cluster boundry
 * @buf: Buffer to store data
 * 
 * Return: Number of bytes read, can differ from @bytes. Negative value on errors.
 * 
 */
int qcow2_readChunk(qcow2_state *c, uint64_t offset, uint64_t bytes, char *buf) {
  /*
    https://github.com/qemu/qemu/blob/master/docs/interop/qcow2.txt

    Given an offset into the virtual disk, the offset into the image file can be
    obtained as follows:

    l2_entries = (cluster_size / sizeof(uint64_t))        [*]

    l2_index = (offset / cluster_size) % l2_entries
    l1_index = (offset / cluster_size) / l2_entries

    l2_table = load_cluster(l1_table[l1_index]);
    cluster_offset = l2_table[l2_index];

    return cluster_offset + (offset % cluster_size)

    [*] this changes if Extended L2 Entries are enabled, see next section
  */

  uint64_t offset_in_cluster = offset_into_cluster(c, offset);
  //printf("offset_in_cluster: %" PRIu64 "\n", offset_in_cluster);

  int num_bytes = c->cluster_size - offset_in_cluster; // number of bytes available in cluster
  if( bytes < num_bytes) num_bytes = bytes; // We don't need all available bytes

  //num_bytes = 1;
  //printf("bytes %" PRIu64 "\n", bytes);
  //printf("Num bytes %i\n", num_bytes);

  int l1_index = offset_to_l1_index(c, offset);
  if( l1_index > c->_header.l1_size ) {
    printf("l1_index > l1_size");
    return -1;
  }
  //printf("l1_index: %i\n", l1_index);
  uint64_t l2_offset = c->l1_table[l1_index] & L1_OFFSET_MASK; // Offset only use bits 9-55
  //printf("l2_offset: %" PRIu64 "\n", l2_offset);

  if(l2_offset == 0) {
    /* 
      L2 offset is 0 this is because data for this location is not stored in this file.
      Either because the data is store in a backing file or because the data at tis location 
      has not been written.

      Because this software is suppose to recover data from a QCow2 file where the backing file 
      is lost we just ignore the backing file part and return a 0 and that we have read 1 byte.
    */
    buf[0] = 0;
    return 1;
  }

  uint64_t *l2_table = qcow2_l2_load(c, l2_offset);

  if( l2_table == NULL) {
    printf("Failed to read L2 table, NULL\n");
    return -1;
  }

  int l2_index = offset_to_l2_index(c, offset);
  //printf("l2_index: %i\n", l2_index);

  if( l2_index > c->l2_size ) {
    printf("l2_index > l2_size");
    return -1;
  }

  uint64_t cluster_offset = l2_table[l2_index];
  //printf("cluster_offset: %" PRIu64 "\n", cluster_offset);
  cluster_offset &= L2_OFFSET_MASK;
  //printf("cluster_offset: %" PRIu64 "\n", cluster_offset);

  if(cluster_offset == 0) {
    /* 
      Cluster offset is 0 this is because data for this location is not stored in this file.
      Either because the data is store in a backing file or because the data at tis location 
      has not been written.

      Because this software is suppose to recover data from a QCow2 file where the backing file 
      is lost we just ignore the backing file part and return a 0 and that we have read 1 byte.
    */
    buf[0] = 0;
    return 1;
  }

  /****************/
  uint64_t b_offset = cluster_offset + offset_in_cluster;
  //printf("Loading byte at offset: %"PRIu64"\n", b_offset);

  if( fseek(c->fp, b_offset, SEEK_SET) != 0 ) {
      printf("Failed to read byte (fseek)\n");
      return -1;
  }
  if( fread(buf, num_bytes, 1, c->fp) != 1 ) {
    printf("Failed to read bytes %i (fread)\n", num_bytes);
    return -1;
  }

  //buf = be64_to_cpu(buf);

  //printf("Byte value %#02x\n", buf);

  return num_bytes;
}

/**
 * @offset: position in bytes to start reading
 * @bytes: number of bytes to read
 * @buf: the buffer to store data
 * 
 */
int qcow2_read(uint64_t offset, uint64_t bytes, char *buf) {
  //printf("read offset: %" PRIu64 "\n", offset);
  //printf("read bytes : %" PRIu64 "\n", bytes);

  char *buf_ptr = buf;
  uint64_t bytes_left = bytes;
  int bytes_read = 0;
  while( bytes_left) {
    //printf("Bytes left: %" PRIu64 "\n", bytes_left);
    bytes_read = qcow2_readChunk(&c, offset, bytes_left, buf_ptr);
    if( bytes_read < 0 || bytes_read > c.cluster_size) {
      printf("Failed to readBytes %i\n", bytes_read);
      return 1;
    }
    buf_ptr += bytes_read;
    offset += bytes_read;
    bytes_left -= bytes_read;
    if( bytes_left <= 0 ) break;
  }
  return 0;
}

char * qcow2_getBackingFile(){
    return c.backingFile;
}
