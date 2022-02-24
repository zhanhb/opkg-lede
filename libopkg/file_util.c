/* file_util.c - convenience routines for common stat operations

   Copyright (C) 2009 Ubiq Technologies <graham.gower@gmail.com>

   Carl D. Worth
   Copyright (C) 2001 University of Southern California

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
*/

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>

#include "sprintf_alloc.h"
#include "file_util.h"
#include <libubox/md5.h>
#include "libbb/libbb.h"

#include "sha256.h"

int file_exists(const char *file_name)
{
	struct stat st;

	if (stat(file_name, &st) == -1)
		return 0;

	return 1;
}

int file_is_dir(const char *file_name)
{
	struct stat st;

	if (stat(file_name, &st) == -1)
		return 0;

	return S_ISDIR(st.st_mode);
}

/* read a single line from a file, stopping at a newline or EOF.
   If a newline is read, it will appear in the resulting string.
   Return value is a malloc'ed char * which should be freed at
   some point by the caller.

   Return value is NULL if the file is at EOF when called.
*/
char *file_read_line_alloc(FILE * fp)
{
	size_t buf_len, line_size = 0;
	char buf[BUFSIZ];
	char *line = NULL;
	int got_nl = 0;
	buf[BUFSIZ - 1] = '\0';

	while (fgets(buf, BUFSIZ, fp)) {
		buf_len = strlen(buf);
		if (buf_len > 0 && buf[buf_len - 1] == '\n') {
			buf[--buf_len] = '\0';
			got_nl = 1;
		}
		if (line) {
			line = xrealloc(line, line_size + buf_len + 1);
			strcpy(&line[line_size], buf);
			line_size += buf_len;
		} else {
			line_size = buf_len;
			line = xstrdup(buf);
		}
		if (got_nl)
			break;
	}

	return line;
}

int file_move(const char *src, const char *dest)
{
	int err;

	err = rename(src, dest);
	if (err == -1) {
		if (errno == EXDEV) {
			/* src & dest live on different file systems */
			err = file_copy(src, dest);
			if (err == 0)
				unlink(src);
		} else {
			opkg_perror(ERROR, "Failed to rename %s to %s",
				    src, dest);
		}
	}

	return err;
}

int file_copy(const char *src, const char *dest)
{
	int err;

	err = copy_file(src, dest, FILEUTILS_FORCE | FILEUTILS_PRESERVE_STATUS);
	if (err)
		opkg_msg(ERROR, "Failed to copy file %s to %s.\n", src, dest);

	return err;
}

int file_mkdir_hier(const char *path, long mode)
{
	return make_directory(path, mode, FILEUTILS_RECUR);
}


static int hex2bin(unsigned char x)
{
	if (x >= 'a' && x <= 'f')
		return x - 'a' + 10;
	else if (x >= 'A' && x <= 'F')
		return x - 'A' + 10;
	else if (x >= '0' && x <= '9')
		return x - '0';
	else
		return 0;
}

static const unsigned char bin2hex[16] = {
	'0', '1', '2', '3',
	'4', '5', '6', '7',
	'8', '9', 'a', 'b',
	'c', 'd', 'e', 'f'
};

char *file_md5sum_alloc(const char *file_name)
{
	static const int md5sum_bin_len = 16;
	static const int md5sum_hex_len = 32;

	int i, len;
	char *md5sum_hex;
	unsigned char md5sum_bin[md5sum_bin_len];

	len = md5sum(file_name, md5sum_bin);

	if (len < 0) {
		opkg_msg(ERROR, "Could't compute md5sum for %s.\n", file_name);
		return NULL;
	}

	md5sum_hex = xcalloc(1, md5sum_hex_len + 1);

	for (i = 0; i < md5sum_bin_len; i++) {
		md5sum_hex[i * 2] = bin2hex[md5sum_bin[i] >> 4];
		md5sum_hex[i * 2 + 1] = bin2hex[md5sum_bin[i] & 0xf];
	}

	md5sum_hex[md5sum_hex_len] = '\0';

	return md5sum_hex;
}

char *file_sha256sum_alloc(const char *file_name)
{
	static const int sha256sum_bin_len = 32;
	static const int sha256sum_hex_len = 64;

	int i, err;
	FILE *file;
	char *sha256sum_hex;
	unsigned char sha256sum_bin[sha256sum_bin_len];

	sha256sum_hex = xcalloc(1, sha256sum_hex_len + 1);

	file = fopen(file_name, "r");
	if (file == NULL) {
		opkg_perror(ERROR, "Failed to open file %s", file_name);
		free(sha256sum_hex);
		return NULL;
	}

	err = sha256_stream(file, sha256sum_bin);
	if (err) {
		opkg_msg(ERROR, "Could't compute sha256sum for %s.\n",
			 file_name);
		fclose(file);
		free(sha256sum_hex);
		return NULL;
	}

	fclose(file);

	for (i = 0; i < sha256sum_bin_len; i++) {
		sha256sum_hex[i * 2] = bin2hex[sha256sum_bin[i] >> 4];
		sha256sum_hex[i * 2 + 1] = bin2hex[sha256sum_bin[i] & 0xf];
	}

	sha256sum_hex[sha256sum_hex_len] = '\0';

	return sha256sum_hex;
}

char *checksum_bin2hex(const char *src, size_t len)
{
	unsigned char *p;
	static unsigned char buf[65];
	const unsigned char *s = (unsigned char *)src;
	if (!s || len > 32)
		return NULL;

	for (p = buf; len > 0; s++, len--) {
		*p++ = bin2hex[*s / 16];
		*p++ = bin2hex[*s % 16];
	}

	*p = 0;

	return (char *)buf;
}

char *checksum_hex2bin(const char *src, size_t *len)
{
	static unsigned char buf[32];
	size_t n = 0;

	*len = 0;

	if (!src)
		return NULL;

	while (isspace(*src))
		src++;

	if (strlen(src) > sizeof(buf) * 2)
		return NULL;

	while (*src) {
		if (n >= sizeof(buf) || !isxdigit(src[0]) || !isxdigit(src[1]))
			return NULL;

		buf[n++] = hex2bin(src[0]) * 16 + hex2bin(src[1]);
		src += 2;
	}

	*len = n;
	return n ? (char *)buf : NULL;
}

int rm_r(const char *path)
{
	int ret = 0;
	DIR *dir;
	struct dirent *dent;

	if (path == NULL) {
		opkg_perror(ERROR, "Missing directory parameter");
		return -1;
	}

	dir = opendir(path);
	if (dir == NULL) {
		opkg_perror(ERROR, "Failed to open dir %s", path);
		return -1;
	}

	if (fchdir(dirfd(dir)) == -1) {
		opkg_perror(ERROR, "Failed to change to dir %s", path);
		closedir(dir);
		return -1;
	}

	while (1) {
		errno = 0;
		if ((dent = readdir(dir)) == NULL) {
			if (errno) {
				opkg_perror(ERROR, "Failed to read dir %s",
					    path);
				ret = -1;
			}
			break;
		}

		if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, ".."))
			continue;

#ifdef _BSD_SOURCE
		if (dent->d_type == DT_DIR) {
			if ((ret = rm_r(dent->d_name)) == -1)
				break;
			continue;
		} else if (dent->d_type == DT_UNKNOWN)
#endif
		{
			struct stat st;
			if ((ret = lstat(dent->d_name, &st)) == -1) {
				opkg_perror(ERROR, "Failed to lstat %s",
					    dent->d_name);
				break;
			}
			if (S_ISDIR(st.st_mode)) {
				if ((ret = rm_r(dent->d_name)) == -1)
					break;
				continue;
			}
		}

		if ((ret = unlink(dent->d_name)) == -1) {
			opkg_perror(ERROR, "Failed to unlink %s", dent->d_name);
			break;
		}
	}

	if (chdir("..") == -1) {
		ret = -1;
		opkg_perror(ERROR, "Failed to change to dir %s/..", path);
	}

	if (rmdir(path) == -1) {
		ret = -1;
		opkg_perror(ERROR, "Failed to remove dir %s", path);
	}

	if (closedir(dir) == -1) {
		ret = -1;
		opkg_perror(ERROR, "Failed to close dir %s", path);
	}

	return ret;
}

static int urlencode_is_specialchar(char c)
{
	switch (c) {
	case ':':
	case '?':
	case '#':
	case '[':
	case ']':
	case '@':
	case '!':
	case '$':
	case '&':
	case '\'':
	case '(':
	case ')':
	case '*':
	case '+':
	case ',':
	case ';':
	case '=':
	case '%':
		return 1;

	default:
		return 0;
	}
}

char *urlencode_path(const char *filename)
{
	size_t len = 0;
	const unsigned char *in;
	unsigned char *copy, *out;

	for (in = (unsigned char *)filename; *in != 0; in++)
		len += urlencode_is_specialchar(*in) ? 3 : 1;

	copy = xcalloc(1, len + 1);

	for (in = (unsigned char *)filename, out = copy; *in != 0; in++) {
		if (urlencode_is_specialchar(*in)) {
			*out++ = '%';
			*out++ = bin2hex[*in / 16];
			*out++ = bin2hex[*in % 16];
		}
		else {
			*out++ = *in;
		}
	}

	return (char *)copy;
}

char *urldecode_path(const char *filename)
{
	unsigned char *copy = (unsigned char *)xstrdup(filename);
	unsigned char *in, *out;

	for (in = copy, out = copy; *in != 0; in++) {
		if (*in == '%' && isxdigit(in[1]) && isxdigit(in[2])) {
			*out++ = hex2bin(in[1]) * 16 + hex2bin(in[2]);
			in += 2;
		}
		else {
			*out++ = *in;
		}
	}

	*out = 0;

	return (char *)copy;
}
