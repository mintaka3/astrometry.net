/*
  This file is part of the Astrometry.net suite.
  Copyright 2007-2008 Dustin Lang.

  The Astrometry.net suite is free software; you can redistribute
  it and/or modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation, version 2.

  The Astrometry.net suite is distributed in the hope that it will be
  useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with the Astrometry.net suite ; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
*/

#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <string.h>
#include <assert.h>

#include "keywords.h"
#include "fitsbin.h"
#include "fitsioutils.h"
#include "ioutils.h"
#include "fitsfile.h"
#include "errors.h"
#include "an-endian.h"
#include "tic.h"
#include "log.h"

// For in-memory: storage of previously-written extensions.
struct fitsext {
	qfits_header* header;
	char* tablename;
	bl* items;
};
typedef struct fitsext fitsext_t;


FILE* fitsbin_get_fid(fitsbin_t* fb) {
    return fb->fid;
}

static int nchunks(fitsbin_t* fb) {
    return bl_size(fb->chunks);
}

static fitsbin_chunk_t* get_chunk(fitsbin_t* fb, int i) {
    if (i >= bl_size(fb->chunks)) {
        ERROR("Attempt to get chunk %i from a fitsbin with only %i chunks",
              i, bl_size(fb->chunks));
        return NULL;
    }
    if (i < 0) {
        ERROR("Attempt to get fitsbin chunk %i", i);
        return NULL;
    }
    return bl_access(fb->chunks, i);
}

static fitsbin_t* new_fitsbin(const char* fn) {
	fitsbin_t* fb;
	fb = calloc(1, sizeof(fitsbin_t));
	if (!fb)
		return NULL;
    fb->chunks = bl_new(4, sizeof(fitsbin_chunk_t));
	if (!fn)
		// Can't make it NULL or qfits freaks out.
		fb->filename = strdup("");
	else
		fb->filename = strdup(fn);
	return fb;
}

static bool in_memory(fitsbin_t* fb) {
	return fb->inmemory;
}


static void free_chunk(fitsbin_chunk_t* chunk) {
    if (!chunk) return;
	free(chunk->tablename_copy);
    if (chunk->header)
        qfits_header_destroy(chunk->header);
	if (chunk->map) {
		if (munmap(chunk->map, chunk->mapsize)) {
			SYSERROR("Failed to munmap fitsbin chunk");
		}
	}
}

void fitsbin_chunk_init(fitsbin_chunk_t* chunk) {
    memset(chunk, 0, sizeof(fitsbin_chunk_t));
}

void fitsbin_chunk_clean(fitsbin_chunk_t* chunk) {
    free_chunk(chunk);
}

void fitsbin_chunk_reset(fitsbin_chunk_t* chunk) {
    fitsbin_chunk_clean(chunk);
    fitsbin_chunk_init(chunk);
}

fitsbin_chunk_t* fitsbin_get_chunk(fitsbin_t* fb, int chunk) {
    return get_chunk(fb, chunk);
}

int fitsbin_n_chunks(fitsbin_t* fb) {
    return nchunks(fb);
}

fitsbin_chunk_t* fitsbin_add_chunk(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    chunk = bl_append(fb->chunks, chunk);
    chunk->tablename_copy = strdup(chunk->tablename);
    chunk->tablename = chunk->tablename_copy;
    return chunk;
}

off_t fitsbin_get_data_start(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    return chunk->header_end;
}

int fitsbin_close(fitsbin_t* fb) {
    int i;
    int rtn = 0;
	if (!fb) return rtn;
    if (fb->fid) {
		if (fclose(fb->fid)) {
			SYSERROR("Error closing fitsbin file");
            rtn = -1;
        }
    }
    if (fb->primheader)
        qfits_header_destroy(fb->primheader);
    for (i=0; i<nchunks(fb); i++) {
		if (in_memory(fb)) {
			free(get_chunk(fb, i)->data);
		}
        free_chunk(get_chunk(fb, i));
	}
    free(fb->filename);
    if (fb->chunks)
        bl_free(fb->chunks);

	if (in_memory(fb)) {
		for (i=0; i<bl_size(fb->extensions); i++) {
			fitsext_t* ext = bl_access(fb->extensions, i);
			bl_free(ext->items);
			qfits_header_destroy(ext->header);
			free(ext->tablename);
		}
		bl_free(fb->extensions);
		bl_free(fb->items);
	}

	free(fb);
    return rtn;
}

int fitsbin_write_primary_header(fitsbin_t* fb) {
	if (in_memory(fb)) return 0;
    return fitsfile_write_primary_header(fb->fid, fb->primheader,
                                         &fb->primheader_end, fb->filename);
}

int fitsbin_write_primary_header_to(fitsbin_t* fb, FILE* fid) {
	off_t end;
	return fitsfile_write_primary_header(fid, fb->primheader, &end, "");
}

qfits_header* fitsbin_get_primary_header(const fitsbin_t* fb) {
    return fb->primheader;
}

int fitsbin_fix_primary_header(fitsbin_t* fb) {
	if (in_memory(fb)) return 0;
    return fitsfile_fix_primary_header(fb->fid, fb->primheader,
                                       &fb->primheader_end, fb->filename);
}

qfits_header* fitsbin_get_chunk_header(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    qfits_table* table;
    int tablesize;
    qfits_header* hdr;
    int ncols = 1;
    char* fn = NULL;

    if (chunk->header)
        return chunk->header;

	// Create the new header.

    if (fb)
        fn = fb->filename;
	if (!fn)
		fn = "";
	// the table header
	tablesize = chunk->itemsize * chunk->nrows * ncols;
	table = qfits_table_new(fn, QFITS_BINTABLE, tablesize, ncols, chunk->nrows);
	assert(table);
    qfits_col_fill(table->col, chunk->itemsize, 0, 1, TFITS_BIN_TYPE_A,
				   chunk->tablename, "", "", "", 0, 0, 0, 0, 0);
    hdr = qfits_table_ext_header_default(table);
    qfits_table_close(table);
    chunk->header = hdr;
    return hdr;
}

static int write_chunk(fitsbin_t* fb, fitsbin_chunk_t* chunk, int flipped) {
    int N;
    if (fitsbin_write_chunk_header(fb, chunk)) {
        return -1;
    }
    N = chunk->nrows;
    if (!flipped) {
        if (fitsbin_write_items(fb, chunk, chunk->data, chunk->nrows))
            return -1;
    } else {
        // endian-flip words of the data of length "flipped", write them,
        // then flip them back to the way they were.

        // NO, copy to temp array, flip it, write it.

        // this is slow, but it won't be run very often...

        int i, j;
        int nper = chunk->itemsize / flipped;
        char tempdata[chunk->itemsize];
        assert(chunk->itemsize >= flipped);
        assert(nper * flipped == chunk->itemsize);
        for (i=0; i<N; i++) {
            // copy it...
            memcpy(tempdata, chunk->data + i*chunk->itemsize, chunk->itemsize);
            // swap it...
            for (j=0; j<nper; j++)
                endian_swap(tempdata + j*flipped, flipped);
            // write it...
            fitsbin_write_item(fb, chunk, tempdata);
        }
    }
    chunk->nrows -= N;
    if (fitsbin_fix_chunk_header(fb, chunk)) {
        return -1;
    }
    return 0;
}

int fitsbin_write_chunk(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    return write_chunk(fb, chunk, 0);
}

int fitsbin_write_chunk_to(fitsbin_t* fb, fitsbin_chunk_t* chunk, FILE* fid) {
    if (fitsbin_write_chunk_header_to(fb, chunk, fid) ||
		fitsbin_write_items_to(chunk, chunk->data, chunk->nrows, fid))
        return -1;
	return 0;
}

int fitsbin_write_chunk_flipped(fitsbin_t* fb, fitsbin_chunk_t* chunk,
                                int wordsize) {
    return write_chunk(fb, chunk, wordsize);
}

int fitsbin_write_chunk_header(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    qfits_header* hdr;
    hdr = fitsbin_get_chunk_header(fb, chunk);
	if (in_memory(fb)) return 0;
    if (fitsfile_write_header(fb->fid, hdr,
                              &chunk->header_start, &chunk->header_end,
                              -1, fb->filename)) {
        return -1;
    }
	return 0;
}

int fitsbin_write_chunk_header_to(fitsbin_t* fb, fitsbin_chunk_t* chunk, FILE* fid) {
	off_t start, end;
    qfits_header* hdr;
    hdr = fitsbin_get_chunk_header(fb, chunk);
    if (fitsfile_write_header(fid, hdr, &start, &end, -1, ""))
        return -1;
	return 0;
}

int fitsbin_fix_chunk_header(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    // update NAXIS2 to reflect the number of rows written.
    fits_header_mod_int(chunk->header, "NAXIS2", chunk->nrows, NULL);
	//if (in_memory(fb)) return 0;

	// HACK -- leverage the fact that this is the last function called for each chunk...
	if (in_memory(fb)) {
		// Save this chunk.
		fitsext_t ext;
		// table, header, items
		if (!fb->extensions)
			fb->extensions = bl_new(4, sizeof(fitsext_t));

		//ext.table = 
		ext.header = qfits_header_copy(chunk->header);
		ext.items = fb->items;
		ext.tablename = strdup(chunk->tablename);
		bl_append(fb->extensions, &ext);
		fb->items = NULL;
		return 0;
	}

    if (fitsfile_fix_header(fb->fid, chunk->header,
                            &chunk->header_start, &chunk->header_end,
                            -1, fb->filename)) {
        return -1;
    }
	return 0;
}

int fitsbin_write_items_to(fitsbin_chunk_t* chunk, void* data, int N, FILE* fid) {
	if (fwrite(data, chunk->itemsize, N, fid) != N) {
		SYSERROR("Failed to write %i items", N);
		return -1;
	}
    return 0;
}

int fitsbin_write_items(fitsbin_t* fb, fitsbin_chunk_t* chunk, void* data, int N) {
	if (in_memory(fb)) {
		int i;
		char* src = data;
		if (!fb->items)
			fb->items = bl_new(1024, chunk->itemsize);
		for (i=0; i<N; i++) {
			bl_append(fb->items, src);
			src += chunk->itemsize;
		}
	} else {
		if (fitsbin_write_items_to(chunk, data, N, fb->fid))
			return -1;
	}
    chunk->nrows += N;
    return 0;
}

int fitsbin_write_item(fitsbin_t* fb, fitsbin_chunk_t* chunk, void* data) {
    return fitsbin_write_items(fb, chunk, data, 1);
}

static int read_chunk(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    struct timeval tv1, tv2;
    int tabstart, tabsize, ext;
    size_t expected = 0;
	int mode, flags;
	off_t mapstart;
	int mapoffset;
    qfits_table* table;
    int table_nrows;
    int table_rowsize;
	fitsext_t* inmemext = NULL;

	if (in_memory(fb)) {
		int i;
		bool gotit = FALSE;
		for (i=0; i<bl_size(fb->extensions); i++) {
			inmemext = bl_access(fb->extensions, i);
			if (strcasecmp(inmemext->tablename, chunk->tablename))
				continue;
			// found it!
			gotit = TRUE;
			break;
		}
		if (!gotit && chunk->required) {
			ERROR("Couldn't find table \"%s\"", chunk->tablename);
			return -1;
		}
		table_nrows = bl_size(inmemext->items);
		table_rowsize = bl_datasize(inmemext->items);
		chunk->header = qfits_header_copy(inmemext->header);

	} else {

		gettimeofday(&tv1, NULL);
		if (fits_find_table_column(fb->filename, chunk->tablename,
								   &tabstart, &tabsize, &ext)) {
			if (chunk->required)
				ERROR("Couldn't find table \"%s\" in file \"%s\"",
					  chunk->tablename, fb->filename);
			return -1;
		}
		gettimeofday(&tv2, NULL);
		debug("fits_find_table_column(%s) took %g ms\n", chunk->tablename, millis_between(&tv1, &tv2));

		chunk->header = qfits_header_readext(fb->filename, ext);
		if (!chunk->header) {
			ERROR("Couldn't read FITS header from file \"%s\" extension %i", fb->filename, ext);
			return -1;
		}

		table = qfits_table_open(fb->filename, ext);
		table_nrows = table->nr;
		table_rowsize = table->tab_w;
		qfits_table_close(table);
	}

    if (!chunk->itemsize)
        chunk->itemsize = table_rowsize;
    if (!chunk->nrows)
        chunk->nrows = table_nrows;

    if (chunk->callback_read_header &&
        chunk->callback_read_header(fb, chunk)) {
        ERROR("fitsbin callback_read_header failed");
        return -1;
    }

    if (chunk->nrows != table_nrows) {
        ERROR("Table %s in file %s: expected %i data items (ie, rows), found %i",
              chunk->tablename, fb->filename, chunk->nrows, table_nrows);
        return -1;
    }

    if (chunk->itemsize != table_rowsize) {
        ERROR("Table %s in file %s: expected data size %i (ie, row width in bytes), found %i",
              chunk->tablename, fb->filename, chunk->itemsize, table_rowsize);
        return -1;
    }

    expected = chunk->itemsize * chunk->nrows;
	if (in_memory(fb)) {
		int i;
		chunk->data = malloc(expected);
		for (i=0; i<chunk->nrows; i++) {
			memcpy(((char*)chunk->data) + i * chunk->itemsize,
				   bl_access(inmemext->items, i), chunk->itemsize);
		}
		// delete inmemext->items ?

	} else {

		if (fits_bytes_needed(expected) != tabsize) {
			ERROR("Expected table size (%i => %i FITS blocks) is not equal to "
				  "size of table \"%s\" (%i FITS blocks).",
				  (int)expected, fits_blocks_needed(expected),
				  chunk->tablename, tabsize / FITS_BLOCK_SIZE);
			return -1;
		}
		get_mmap_size(tabstart, tabsize, &mapstart, &(chunk->mapsize), &mapoffset);
		mode = PROT_READ;
		flags = MAP_SHARED;
		chunk->map = mmap(0, chunk->mapsize, mode, flags, fileno(fb->fid), mapstart);
		if (chunk->map == MAP_FAILED) {
			SYSERROR("Couldn't mmap file \"%s\"", fb->filename);
			chunk->map = NULL;
			return -1;
		}
		chunk->data = chunk->map + mapoffset;
	}
    return 0;
}

int fitsbin_read_chunk(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    if (read_chunk(fb, chunk))
        return -1;
    fitsbin_add_chunk(fb, chunk);
    return 0;
}

int fitsbin_read(fitsbin_t* fb) {
    int i;

    for (i=0; i<nchunks(fb); i++) {
        fitsbin_chunk_t* chunk = get_chunk(fb, i);
        if (read_chunk(fb, chunk)) {
            if (chunk->required)
                goto bailout;
        }
    }
    return 0;

 bailout:
    return -1;
}

fitsbin_t* fitsbin_open(const char* fn) {
    fitsbin_t* fb;
	if (!qfits_is_fits(fn)) {
        ERROR("File \"%s\" is not FITS format.", fn);
        return NULL;
	}
    fb = new_fitsbin(fn);
    if (!fb)
        return fb;
	fb->fid = fopen(fn, "rb");
	if (!fb->fid) {
		SYSERROR("Failed to open file \"%s\"", fn);
        goto bailout;
	}
    fb->primheader = qfits_header_read(fn);
    if (!fb->primheader) {
        ERROR("Couldn't read FITS header from file \"%s\"", fn);
        goto bailout;
    }
    return fb;
 bailout:
    fitsbin_close(fb);
    return NULL;
}

fitsbin_t* fitsbin_open_in_memory() {
    fitsbin_t* fb;

    fb = new_fitsbin(NULL);
    if (!fb)
        return NULL;
    fb->primheader = qfits_table_prim_header_default();
	fb->inmemory = TRUE;
	return fb;
}

int fitsbin_switch_to_reading(fitsbin_t* fb) {
	return 0;
}

fitsbin_t* fitsbin_open_for_writing(const char* fn) {
    fitsbin_t* fb;

    fb = new_fitsbin(fn);
    if (!fb)
        return NULL;
    fb->primheader = qfits_table_prim_header_default();
	fb->fid = fopen(fb->filename, "wb");
	if (!fb->fid) {
		SYSERROR("Couldn't open file \"%s\" for output", fb->filename);
        fitsbin_close(fb);
        return NULL;
	}
    return fb;
}

