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
#ifndef _QCOW2_H_
#define _QCOW2_H_

#include <inttypes.h> // uint64_t & PRIu64 for printf

int qcow2_openFile(const char *filename);
char * qcow2_getBackingFile();
void qcow2_closeFile();

uint64_t qcow2_getFileSize();

int qcow2_read(uint64_t offset, uint64_t bytes, char *buf);

#endif
