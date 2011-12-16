#include "fitz.h"
#include "muxps.h"

#include <zlib.h>

xps_part *
xps_new_part(xps_document *doc, char *name, int size)
{
	xps_part *part;

	part = fz_malloc(doc->ctx, sizeof(xps_part));
	part->name = fz_strdup(doc->ctx, name);
	part->size = size;
	part->data = fz_malloc(doc->ctx, size + 1);
	part->data[size] = 0; /* null-terminate for xml parser */

	return part;
}

void
xps_free_part(xps_document *doc, xps_part *part)
{
	fz_free(doc->ctx, part->name);
	fz_free(doc->ctx, part->data);
	fz_free(doc->ctx, part);
}

static inline int getshort(fz_stream *file)
{
	int a = fz_read_byte(file);
	int b = fz_read_byte(file);
	return a | b << 8;
}

static inline int getlong(fz_stream *file)
{
	int a = fz_read_byte(file);
	int b = fz_read_byte(file);
	int c = fz_read_byte(file);
	int d = fz_read_byte(file);
	return a | b << 8 | c << 16 | d << 24;
}

static void *
xps_zip_alloc_items(xps_document *doc, int items, int size)
{
	return fz_malloc_array(doc->ctx, items, size);
}

static void
xps_zip_free(xps_document *doc, void *ptr)
{
	fz_free(doc->ctx, ptr);
}

static int
xps_compare_entries(const void *a0, const void *b0)
{
	xps_entry *a = (xps_entry*) a0;
	xps_entry *b = (xps_entry*) b0;
	return xps_strcasecmp(a->name, b->name);
}

static xps_entry *
xps_find_zip_entry(xps_document *doc, char *name)
{
	int l = 0;
	int r = doc->zip_count - 1;
	while (l <= r)
	{
		int m = (l + r) >> 1;
		int c = xps_strcasecmp(name, doc->zip_table[m].name);
		if (c < 0)
			r = m - 1;
		else if (c > 0)
			l = m + 1;
		else
			return &doc->zip_table[m];
	}
	return NULL;
}

static void
xps_read_zip_entry(xps_document *doc, xps_entry *ent, unsigned char *outbuf)
{
	z_stream stream;
	unsigned char *inbuf;
	int sig;
	int version, general, method;
	int namelength, extralength;
	int code;

	fz_seek(doc->file, ent->offset, 0);

	sig = getlong(doc->file);
	if (sig != ZIP_LOCAL_FILE_SIG)
		fz_throw(doc->ctx, "wrong zip local file signature (0x%x)", sig);

	version = getshort(doc->file);
	general = getshort(doc->file);
	method = getshort(doc->file);
	(void) getshort(doc->file); /* file time */
	(void) getshort(doc->file); /* file date */
	(void) getlong(doc->file); /* crc-32 */
	(void) getlong(doc->file); /* csize */
	(void) getlong(doc->file); /* usize */
	namelength = getshort(doc->file);
	extralength = getshort(doc->file);

	fz_seek(doc->file, namelength + extralength, 1);

	if (method == 0)
	{
		fz_read(doc->file, outbuf, ent->usize);
	}
	else if (method == 8)
	{
		inbuf = fz_malloc(doc->ctx, ent->csize);

		fz_read(doc->file, inbuf, ent->csize);

		memset(&stream, 0, sizeof(z_stream));
		stream.zalloc = (alloc_func) xps_zip_alloc_items;
		stream.zfree = (free_func) xps_zip_free;
		stream.opaque = doc;
		stream.next_in = inbuf;
		stream.avail_in = ent->csize;
		stream.next_out = outbuf;
		stream.avail_out = ent->usize;

		code = inflateInit2(&stream, -15);
		if (code != Z_OK)
			fz_throw(doc->ctx, "zlib inflateInit2 error: %s", stream.msg);
		code = inflate(&stream, Z_FINISH);
		if (code != Z_STREAM_END)
		{
			inflateEnd(&stream);
			fz_throw(doc->ctx, "zlib inflate error: %s", stream.msg);
		}
		code = inflateEnd(&stream);
		if (code != Z_OK)
			fz_throw(doc->ctx, "zlib inflateEnd error: %s", stream.msg);

		fz_free(doc->ctx, inbuf);
	}
	else
	{
		fz_throw(doc->ctx, "unknown compression method (%d)", method);
	}
}

/*
 * Read the central directory in a zip file.
 */

static void
xps_read_zip_dir(xps_document *doc, int start_offset)
{
	int sig;
	int offset, count;
	int namesize, metasize, commentsize;
	int i;

	fz_seek(doc->file, start_offset, 0);

	sig = getlong(doc->file);
	if (sig != ZIP_END_OF_CENTRAL_DIRECTORY_SIG)
		fz_throw(doc->ctx, "wrong zip end of central directory signature (0x%x)", sig);

	(void) getshort(doc->file); /* this disk */
	(void) getshort(doc->file); /* start disk */
	(void) getshort(doc->file); /* entries in this disk */
	count = getshort(doc->file); /* entries in central directory disk */
	(void) getlong(doc->file); /* size of central directory */
	offset = getlong(doc->file); /* offset to central directory */

	doc->zip_count = count;
	doc->zip_table = fz_malloc_array(doc->ctx, count, sizeof(xps_entry));

	fz_seek(doc->file, offset, 0);

	for (i = 0; i < count; i++)
	{
		sig = getlong(doc->file);
		if (sig != ZIP_CENTRAL_DIRECTORY_SIG)
			fz_throw(doc->ctx, "wrong zip central directory signature (0x%x)", sig);

		(void) getshort(doc->file); /* version made by */
		(void) getshort(doc->file); /* version to extract */
		(void) getshort(doc->file); /* general */
		(void) getshort(doc->file); /* method */
		(void) getshort(doc->file); /* last mod file time */
		(void) getshort(doc->file); /* last mod file date */
		(void) getlong(doc->file); /* crc-32 */
		doc->zip_table[i].csize = getlong(doc->file);
		doc->zip_table[i].usize = getlong(doc->file);
		namesize = getshort(doc->file);
		metasize = getshort(doc->file);
		commentsize = getshort(doc->file);
		(void) getshort(doc->file); /* disk number start */
		(void) getshort(doc->file); /* int file atts */
		(void) getlong(doc->file); /* ext file atts */
		doc->zip_table[i].offset = getlong(doc->file);

		doc->zip_table[i].name = fz_malloc(doc->ctx, namesize + 1);
		fz_read(doc->file, (unsigned char*)doc->zip_table[i].name, namesize);
		doc->zip_table[i].name[namesize] = 0;

		fz_seek(doc->file, metasize, 1);
		fz_seek(doc->file, commentsize, 1);
	}

	qsort(doc->zip_table, count, sizeof(xps_entry), xps_compare_entries);
}

static void
xps_find_and_read_zip_dir(xps_document *doc)
{
	unsigned char buf[512];
	int file_size, back, maxback;
	int i, n;

	fz_seek(doc->file, 0, SEEK_END);
	file_size = fz_tell(doc->file);

	maxback = MIN(file_size, 0xFFFF + sizeof buf);
	back = MIN(maxback, sizeof buf);

	while (back < maxback)
	{
		fz_seek(doc->file, file_size - back, 0);
		n = fz_read(doc->file, buf, sizeof buf);
		for (i = n - 4; i > 0; i--)
		{
			if (!memcmp(buf + i, "PK\5\6", 4))
			{
				xps_read_zip_dir(doc, file_size - back + i);
				return;
			}
		}

		back += sizeof buf - 4;
	}

	fz_throw(doc->ctx, "cannot find end of central directory");
}

/*
 * Read and interleave split parts from a ZIP file.
 */
static xps_part *
xps_read_zip_part(xps_document *doc, char *partname)
{
	char buf[2048];
	xps_entry *ent;
	xps_part *part;
	int count, size, offset, i;
	char *name;

	name = partname;
	if (name[0] == '/')
		name ++;

	/* All in one piece */
	ent = xps_find_zip_entry(doc, name);
	if (ent)
	{
		part = xps_new_part(doc, partname, ent->usize);
		xps_read_zip_entry(doc, ent, part->data);
		return part;
	}

	/* Count the number of pieces and their total size */
	count = 0;
	size = 0;
	while (1)
	{
		sprintf(buf, "%s/[%d].piece", name, count);
		ent = xps_find_zip_entry(doc, buf);
		if (!ent)
		{
			sprintf(buf, "%s/[%d].last.piece", name, count);
			ent = xps_find_zip_entry(doc, buf);
		}
		if (!ent)
			break;
		count ++;
		size += ent->usize;
	}

	/* Inflate the pieces */
	if (count)
	{
		part = xps_new_part(doc, partname, size);
		offset = 0;
		for (i = 0; i < count; i++)
		{
			if (i < count - 1)
				sprintf(buf, "%s/[%d].piece", name, i);
			else
				sprintf(buf, "%s/[%d].last.piece", name, i);
			ent = xps_find_zip_entry(doc, buf);
			xps_read_zip_entry(doc, ent, part->data + offset);
			offset += ent->usize;
		}
		return part;
	}

	fz_throw(doc->ctx, "cannot find part '%s'", partname);
	return NULL;
}

static int
xps_has_zip_part(xps_document *doc, char *name)
{
	char buf[2048];
	if (xps_find_zip_entry(doc, name))
		return 1;
	sprintf(buf, "%s/[0].piece", name);
	if (xps_find_zip_entry(doc, buf));
		return 1;
	sprintf(buf, "%s/[0].last.piece", name);
	if (xps_find_zip_entry(doc, buf));
		return 1;
	return 0;
}

/*
 * Read and interleave split parts from files in the directory.
 */
static xps_part *
xps_read_dir_part(xps_document *doc, char *name)
{
	char buf[2048];
	xps_part *part;
	FILE *file;
	int count, size, offset, i, n;

	fz_strlcpy(buf, doc->directory, sizeof buf);
	fz_strlcat(buf, name, sizeof buf);

	/* All in one piece */
	file = fopen(buf, "rb");
	if (file)
	{
		fseek(file, 0, SEEK_END);
		size = ftell(file);
		fseek(file, 0, SEEK_SET);
		part = xps_new_part(doc, name, size);
		fread(part->data, 1, size, file);
		fclose(file);
		return part;
	}

	/* Count the number of pieces and their total size */
	count = 0;
	size = 0;
	while (1)
	{
		sprintf(buf, "%s%s/[%d].piece", doc->directory, name, count);
		file = fopen(buf, "rb");
		if (!file)
		{
			sprintf(buf, "%s%s/[%d].last.piece", doc->directory, name, count);
			file = fopen(buf, "rb");
		}
		if (!file)
			break;
		count ++;
		fseek(file, 0, SEEK_END);
		size += ftell(file);
		fclose(file);
	}

	/* Inflate the pieces */
	if (count)
	{
		part = xps_new_part(doc, name, size);
		offset = 0;
		for (i = 0; i < count; i++)
		{
			if (i < count - 1)
				sprintf(buf, "%s%s/[%d].piece", doc->directory, name, i);
			else
				sprintf(buf, "%s%s/[%d].last.piece", doc->directory, name, i);
			file = fopen(buf, "rb");
			if (!file)
				fz_throw(doc->ctx, "cannot open file '%s'", buf);
			n = fread(part->data + offset, 1, size - offset, file);
			offset += n;
			fclose(file);
		}
		return part;
	}

	fz_throw(doc->ctx, "cannot find part '%s'", name);
	return NULL;
}

static int
file_exists(xps_document *doc, char *name)
{
	char buf[2048];
	FILE *file;
	fz_strlcpy(buf, doc->directory, sizeof buf);
	fz_strlcat(buf, name, sizeof buf);
	file = fopen(buf, "rb");
	if (file)
	{
		fclose(file);
		return 1;
	}
	return 0;
}

static int
xps_has_dir_part(xps_document *doc, char *name)
{
	char buf[2048];
	if (file_exists(doc, name))
		return 1;
	sprintf(buf, "%s/[0].piece", name);
	if (file_exists(doc, buf));
		return 1;
	sprintf(buf, "%s/[0].last.piece", name);
	if (file_exists(doc, buf));
		return 1;
	return 0;
}

xps_part *
xps_read_part(xps_document *doc, char *partname)
{
	if (doc->directory)
		return xps_read_dir_part(doc, partname);
	return xps_read_zip_part(doc, partname);
}

int
xps_has_part(xps_document *doc, char *partname)
{
	if (doc->directory)
		return xps_has_dir_part(doc, partname);
	return xps_has_zip_part(doc, partname);
}

static xps_document *
xps_open_directory(fz_context *ctx, char *directory)
{
	xps_document *doc;

	doc = fz_malloc_struct(ctx, xps_document);
	memset(doc, 0, sizeof *doc);

	doc->ctx = ctx;
	doc->directory = fz_strdup(ctx, directory);

	fz_try(ctx)
	{
		xps_read_page_list(doc);
	}
	fz_catch(ctx)
	{
		xps_free_context(doc);
		fz_rethrow(ctx);
	}

	return doc;
}

xps_document *
xps_open_stream(fz_stream *file)
{
	fz_context *ctx = file->ctx;
	xps_document *doc;

	doc = fz_malloc_struct(ctx, xps_document);
	memset(doc, 0, sizeof *doc);

	doc->ctx = ctx;
	doc->file = fz_keep_stream(file);

	fz_try(ctx)
	{
		xps_find_and_read_zip_dir(doc);
		xps_read_page_list(doc);
	}
	fz_catch(ctx)
	{
		xps_free_context(doc);
		fz_rethrow(ctx);
	}

	return doc;
}

xps_document *
xps_open_file(fz_context *ctx, char *filename)
{
	char buf[2048];
	fz_stream *file;
	char *p;
	xps_document *doc;

	if (strstr(filename, "/_rels/.rels") || strstr(filename, "\\_rels\\.rels"))
	{
		fz_strlcpy(buf, filename, sizeof buf);
		p = strstr(buf, "/_rels/.rels");
		if (!p)
			p = strstr(buf, "\\_rels\\.rels");
		*p = 0;
		return xps_open_directory(ctx, buf);
	}

	file = fz_open_file(ctx, filename);
	if (!file)
		fz_throw(ctx, "cannot open file '%s': %s", filename, strerror(errno));

	fz_try(ctx)
	{
		doc = xps_open_stream(file);
	}
	fz_catch(ctx)
	{
		fz_close(file);
		fz_throw(ctx, "cannot load document '%s'", filename);
	}
	fz_close(file);
	return doc;
}

void
xps_free_context(xps_document *doc)
{
	xps_font_cache *font, *next;
	int i;

	if (doc->file)
		fz_close(doc->file);

	for (i = 0; i < doc->zip_count; i++)
		fz_free(doc->ctx, doc->zip_table[i].name);
	fz_free(doc->ctx, doc->zip_table);

	font = doc->font_table;
	while (font)
	{
		next = font->next;
		fz_drop_font(doc->ctx, font->font);
		fz_free(doc->ctx, font->name);
		fz_free(doc->ctx, font);
		font = next;
	}

	xps_free_page_list(doc);

	fz_free(doc->ctx, doc->start_part);
	fz_free(doc->ctx, doc->directory);
	fz_free(doc->ctx, doc);
}
