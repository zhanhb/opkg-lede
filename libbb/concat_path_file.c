/* vi: set sw=4 ts=4: */
/*
 * Utility routines.
 *
 * Copyright (C) many different people.  If you wrote this, please
 * acknowledge your work.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

/* concatenate path and file name to new allocation buffer,
 * not addition '/' if path name already have '/'
*/

#include <string.h>
#include "libbb.h"

extern char *concat_path_file(const char *path, const char *filename)
{
	char *outbuf;
	int ends_with_slash;
	size_t path_len, name_len;

	if (!path)
		path = "";
	path_len = strlen(path);
	ends_with_slash = path_len && path[path_len - 1] == '/';
	while (*filename == '/')
		filename++;
	name_len = strlen(filename);
	outbuf = xmalloc(path_len + name_len + !ends_with_slash + 1);
	memcpy(outbuf, path, path_len);
	if (!ends_with_slash) outbuf[path_len++] = '/';
	memcpy(outbuf + path_len, filename, name_len);
	outbuf[path_len + name_len] = 0;
	return outbuf;
}
