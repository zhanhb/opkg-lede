/* pkg_parse.c - the opkg package management system

   Copyright (C) 2009 Ubiq Technologies <graham.gower@gmail.com>

   Steven M. Ayer
   Copyright (C) 2002 Compaq Computer Corporation

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
#include <ctype.h>
#include <unistd.h>

#include "pkg.h"
#include "opkg_utils.h"
#include "pkg_parse.h"
#include "pkg_depends.h"
#include "libbb/libbb.h"

#include "file_util.h"
#include "parse_util.h"

static void parse_status(pkg_t * pkg, const char *sstr)
{
	char sw_str[64], sf_str[64], ss_str[64];

	if (sscanf(sstr, "Status: %63s %63s %63s", sw_str, sf_str, ss_str) != 3) {
		opkg_msg(ERROR, "Failed to parse Status line for %s\n",
			 pkg->name);
		return;
	}

	pkg->state_want = pkg_state_want_from_str(sw_str);
	pkg->state_flag |= pkg_state_flag_from_str(sf_str);
	pkg->state_status = pkg_state_status_from_str(ss_str);
}

static void parse_conffiles(pkg_t * pkg, const char *cstr)
{
	conffile_list_t *cl;
	char file_name[1024], md5sum[85];

	if (sscanf(cstr, "%1023s %84s", file_name, md5sum) != 2) {
		opkg_msg(ERROR, "Failed to parse Conffiles line for %s\n",
			 pkg->name);
		return;
	}

	cl = pkg_get_ptr(pkg, PKG_CONFFILES);

	if (cl)
		conffile_list_append(cl, file_name, md5sum);
}

int parse_version(pkg_t * pkg, const char *vstr)
{
	char *colon, *dup, *rev;

	if (strncmp(vstr, "Version:", 8) == 0)
		vstr += 8;

	while (*vstr && isspace(*vstr))
		vstr++;

	colon = strchr(vstr, ':');
	if (colon) {
		errno = 0;
		pkg_set_int(pkg, PKG_EPOCH, strtoul(vstr, NULL, 10));
		if (errno) {
			opkg_perror(ERROR, "%s: invalid epoch", pkg->name);
		}
		vstr = ++colon;
	}


	dup = xstrdup(vstr);
	rev = strrchr(dup, '-');

	if (rev) {
		*rev++ = '\0';
		pkg_set_string(pkg, PKG_REVISION, rev);
	}

	pkg_set_string(pkg, PKG_VERSION, dup);
	free(dup);

	return 0;
}

static char *parse_architecture(pkg_t *pkg, const char *str)
{
	const char *s = str;
	const char *e;

	while (isspace(*s))
		s++;

	e = s + strlen(s);

	while (e > s && isspace(*e))
		e--;

	return pkg_set_architecture(pkg, s, e - s);
}

static void parse_alternatives(pkg_t *pkg, char *list)
{
	char *item, *tok;
	struct pkg_alternatives *pkg_alts;
	struct pkg_alternative **alts;
	int nalts;

	pkg_alts = pkg_get_ptr(pkg, PKG_ALTERNATIVES);
	if (!pkg_alts) {
		nalts = 0;
		alts = NULL;
	} else {
		nalts = pkg_alts->nalts;
		alts = pkg_alts->alts;
	}

	for (item = strtok_r(list, ",", &tok);
			item;
			item = strtok_r(NULL, ",", &tok)) {
		enum pkg_alternative_field i;
		char *val, *tok1;
		/* the assignment was intended to quash the -Wmaybe-uninitialized warnings */
		int prio = prio;
		char *path = path, *altpath = altpath;

		for (i = PAF_PRIO, val = strtok_r(item, ":", &tok1);
				val && i < __PAF_MAX;
				val = strtok_r(NULL, ":", &tok1), i++) {
			switch (i) {
				case PAF_PRIO:
					prio = atoi(val);
					break;
				case PAF_PATH:
					path = val;
					break;
				case PAF_ALTPATH:
					altpath = val;
					break;
				default:
					break;
			}
		}
		if (!val && i == __PAF_MAX) {
			char *_path, *_altpath;
			struct pkg_alternative *alt;

			/*
			 * - path must be absolute
			 * - altpath must be non-empty
			 */
			if (path[0] != '/' || !altpath[0])
				continue;

			alt = calloc_a(sizeof(*alt),
					&_path, strlen(path) + 1,
					&_altpath, strlen(altpath) + 1);
			if (!alt)
				continue;
			strcpy(_path, path);
			strcpy(_altpath, altpath);
			alt->prio = prio;
			alt->path = _path;
			alt->altpath = _altpath;
			alts = xrealloc(alts, sizeof(*alts) * (nalts + 1));
			alts[nalts++] = alt;
		}
	}

	if (nalts > 0) {
		if (!pkg_alts)
			pkg_alts = xmalloc(sizeof(*pkg_alts));
		pkg_alts->nalts = nalts;
		pkg_alts->alts = alts;
		pkg_set_ptr(pkg, PKG_ALTERNATIVES, pkg_alts);
	}
}

int pkg_parse_line(void *ptr, char *line, uint mask)
{
	pkg_t *pkg = (pkg_t *) ptr;
	abstract_pkg_t *ab_pkg = NULL;
	conffile_list_t *cl;

	/* these flags are a bit hackish... */
	static int reading_conffiles = 0, reading_description = 0;
	static char *description = NULL;
	static size_t description_len = 0;
	int ret = 0;

	/* Exclude globally masked fields. */
	mask |= conf->pfm;

	/* Flip the semantics of the mask. */
	mask ^= PFM_ALL;

	switch (*line) {
	case 'A':
		if ((mask & PFM_ABIVERSION) && is_field("ABIVersion", line))
			pkg_set_string(pkg, PKG_ABIVERSION, line + strlen("ABIVersion") + 1);
		else if ((mask & PFM_ALTERNATIVES) && is_field("Alternatives", line))
			parse_alternatives(pkg, line + strlen("Alternatives") + 1);
		else if ((mask & PFM_ARCHITECTURE) && is_field("Architecture", line))
			parse_architecture(pkg, line + strlen("Architecture") + 1);
		else if ((mask & PFM_AUTO_INSTALLED)
			   && is_field("Auto-Installed", line)) {
			char *tmp = parse_simple("Auto-Installed", line);
			if (strcmp(tmp, "yes") == 0)
				pkg->auto_installed = 1;
			free(tmp);
		}
		break;

	case 'C':
		if ((mask & PFM_CONFFILES) && is_field("Conffiles", line)) {
			reading_conffiles = 1;
			reading_description = 0;

			cl = xcalloc(1, sizeof(*cl));
			conffile_list_init(cl);
			pkg_set_ptr(pkg, PKG_CONFFILES, cl);

			goto dont_reset_flags;
		} else if ((mask & PFM_CONFLICTS)
			   && is_field("Conflicts", line))
			parse_deplist(pkg, CONFLICTS, line + strlen("Conflicts") + 1);
		break;

	case 'D':
		if ((mask & PFM_DESCRIPTION) && is_field("Description", line)) {
			description = parse_simple("Description", line);
			description_len = description ? strlen(description) : 0;
			reading_conffiles = 0;
			reading_description = 1;
			goto dont_reset_flags;
		} else if ((mask & PFM_DEPENDS) && is_field("Depends", line))
			parse_deplist(pkg, DEPEND, line + strlen("Depends") + 1);
		break;

	case 'E':
		if ((mask & PFM_ESSENTIAL) && is_field("Essential", line)) {
			char *tmp = parse_simple("Essential", line);
			if (strcmp(tmp, "yes") == 0)
				pkg->essential = 1;
			free(tmp);
		}
		break;

	case 'F':
		if ((mask & PFM_FILENAME) && is_field("Filename", line))
			pkg_set_string(pkg, PKG_FILENAME, line + strlen("Filename") + 1);
		break;

	case 'I':
		if ((mask & PFM_INSTALLED_SIZE)
		    && is_field("Installed-Size", line)) {
			pkg_set_int(pkg, PKG_INSTALLED_SIZE, strtoul(line + strlen("Installed-Size") + 1, NULL, 0));
		} else if ((mask & PFM_INSTALLED_TIME)
			   && is_field("Installed-Time", line)) {
			pkg_set_int(pkg, PKG_INSTALLED_TIME, strtoul(line + strlen("Installed-Time") + 1, NULL, 0));
		}
		break;

	case 'M':
		if ((mask & PFM_MD5SUM) && (is_field("MD5sum:", line) || is_field("MD5Sum:", line)))
			pkg_set_md5(pkg, line + strlen("MD5sum") + 1);
		else if ((mask & PFM_MAINTAINER)
			 && is_field("Maintainer", line))
			pkg_set_string(pkg, PKG_MAINTAINER, line + strlen("Maintainer") + 1);
		break;

	case 'P':
		if ((mask & PFM_PACKAGE) && is_field("Package", line)) {
			pkg->name = parse_simple("Package", line);
			ab_pkg = abstract_pkg_fetch_by_name(pkg->name);

			if (ab_pkg && (ab_pkg->state_flag & SF_NEED_DETAIL)) {
				if (!(pkg->state_flag & SF_NEED_DETAIL)) {
					opkg_msg(DEBUG, "propagating abpkg flag to pkg %s\n", pkg->name);
					pkg->state_flag |= SF_NEED_DETAIL;
				}
			}
		}
		else if ((mask & PFM_PRIORITY) && is_field("Priority", line))
			pkg_set_string(pkg, PKG_PRIORITY, line + strlen("Priority") + 1);
		else if ((mask & PFM_PROVIDES) && is_field("Provides", line))
			parse_providelist(pkg, line + strlen("Provides") + 1);
		else if ((mask & PFM_PRE_DEPENDS)
			 && is_field("Pre-Depends", line))
			parse_deplist(pkg, PREDEPEND, line + strlen("Pre-Depends") + 1);
		break;

	case 'R':
		if ((mask & PFM_RECOMMENDS) && is_field("Recommends", line))
			parse_deplist(pkg, RECOMMEND, line + strlen("Recommends") + 1);
		else if ((mask & PFM_REPLACES) && is_field("Replaces", line))
			parse_replacelist(pkg, line + strlen("Replaces") + 1);
		break;

	case 'S':
		if ((mask & PFM_SECTION) && is_field("Section", line))
			pkg_set_string(pkg, PKG_SECTION, line + strlen("Section") + 1);
		else if ((mask & PFM_SHA256SUM) && is_field("SHA256sum", line))
			pkg_set_sha256(pkg, line + strlen("SHA256sum") + 1);
		else if ((mask & PFM_SIZE) && is_field("Size", line)) {
			pkg_set_int(pkg, PKG_SIZE, strtoul(line + strlen("Size") + 1, NULL, 0));
		} else if ((mask & PFM_SOURCE) && is_field("Source", line))
			pkg_set_string(pkg, PKG_SOURCE, line + strlen("Source") + 1);
		else if ((mask & PFM_STATUS) && is_field("Status", line))
			parse_status(pkg, line);
		else if ((mask & PFM_SUGGESTS) && is_field("Suggests", line))
			parse_deplist(pkg, SUGGEST, line + strlen("Suggests") + 1);
		break;

	case 'T':
		if ((mask & PFM_TAGS) && is_field("Tags", line))
			pkg_set_string(pkg, PKG_TAGS, line + strlen("Tags") + 1);
		break;

	case 'V':
		if ((mask & PFM_VERSION) && is_field("Version", line))
			parse_version(pkg, line);
		break;

	case ' ':
		if ((mask & PFM_DESCRIPTION) && reading_description) {
			size_t line_len = strlen(line);
			int istty = isatty(1) != 0;
			size_t len = description_len + istty + line_len;

			description = description ? xrealloc(description, len + 1) : xmalloc(len + 1);

			if (istty)
				description[description_len++] = '\n';

			strcpy(description + description_len, line);
			description_len = len;
			goto dont_reset_flags;
		} else if ((mask & PFM_CONFFILES) && reading_conffiles) {
			parse_conffiles(pkg, line);
			goto dont_reset_flags;
		}

		/* FALLTHROUGH */
	default:
		/* For package lists, signifies end of package. */
		if (line_is_blank(line)) {
			ret = 1;
			break;
		}
	}

	if (reading_description && description) {
		pkg_set_string(pkg, PKG_DESCRIPTION, description);
		free(description);
		reading_description = 0;
		description = NULL;
		description_len = 0;
	}

	reading_conffiles = 0;

dont_reset_flags:

	return ret;
}

int pkg_parse_from_stream(pkg_t * pkg, FILE * fp, uint mask)
{
	int ret;
	char *buf;
	const size_t len = 4096;

	buf = xmalloc(len);
	ret =
	    parse_from_stream_nomalloc(pkg_parse_line, pkg, fp, mask, &buf,
				       len);
	free(buf);

	if (pkg->name == NULL) {
		/* probably just a blank line */
		ret = 1;
	}

	return ret;
}
