/* opkg_hash.c - the opkg package management system

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

#include "hash_table.h"
#include "pkg.h"
#include "opkg_message.h"
#include "pkg_depends.h"
#include "pkg_vec.h"
#include "pkg_hash.h"
#include "parse_util.h"
#include "pkg_parse.h"
#include "opkg_utils.h"
#include "opkg_cmd.h"
#include "sprintf_alloc.h"
#include "file_util.h"
#include "libbb/libbb.h"
#include "libbb/gzip.h"

void pkg_hash_init(void)
{
	hash_table_init("pkg-hash", &conf->pkg_hash,
			OPKG_CONF_DEFAULT_HASH_LEN);
}

static void free_pkgs(const char *key, void *entry, void *data)
{
	int i;
	abstract_pkg_t *ab_pkg;

	/* Each entry in the hash table is an abstract package, which contains
	 * a list of packages that provide the abstract package.
	 */

	ab_pkg = (abstract_pkg_t *) entry;

	if (ab_pkg->pkgs) {
		for (i = 0; i < ab_pkg->pkgs->len; i++) {
			pkg_deinit(ab_pkg->pkgs->pkgs[i]);
			free(ab_pkg->pkgs->pkgs[i]);
		}
	}

	abstract_pkg_vec_free(ab_pkg->provided_by);
	abstract_pkg_vec_free(ab_pkg->replaced_by);
	pkg_vec_free(ab_pkg->pkgs);
	abstract_pkg_vec_free(ab_pkg->depended_upon_by);
	free(ab_pkg->name);
	free(ab_pkg);
}

void pkg_hash_deinit(void)
{
	hash_table_foreach(&conf->pkg_hash, free_pkgs, NULL);
	hash_table_deinit(&conf->pkg_hash);
}

int
pkg_hash_add_from_file(const char *file_name,
		       pkg_src_t * src, pkg_dest_t * dest, int is_status_file, int state_flags,
		       void (*cb)(pkg_t *, void *), void *priv)
{
	pkg_t *pkg;
	FILE *fp;
	char *buf;
	const size_t len = 4096;
	int ret = 0;
	struct gzip_handle zh;

	if (src && src->gzip) {
		fp = gzip_fdopen(&zh, file_name);
	} else {
		fp = fopen(file_name, "r");
	}

	if (fp == NULL) {
		opkg_perror(ERROR, "Failed to open %s", file_name);
		return -1;
	}

	buf = xmalloc(len);

	do {
		pkg = pkg_new();
		pkg->src = src;
		pkg->dest = dest;
		pkg->state_flag |= state_flags;

		ret = parse_from_stream_nomalloc(pkg_parse_line, pkg, fp, 0,
						 &buf, len);

		if (pkg->name == NULL) {
			/* probably just a blank line */
			ret = 1;
		}

		if (ret) {
			pkg_deinit(pkg);
			free(pkg);
			if (ret == -1)
				break;
			if (ret == 1)
				/* Probably a blank line, continue parsing. */
				ret = 0;
			continue;
		}

		if (!(pkg->state_flag & SF_NEED_DETAIL)) {
			//opkg_msg(DEBUG, "Package %s is unrelated, ignoring.\n", pkg->name);
			pkg_deinit(pkg);
			free(pkg);
			continue;
		}

		if (!pkg_get_architecture(pkg) || !pkg_get_arch_priority(pkg)) {
			char *version_str = pkg_version_str_alloc(pkg);
			opkg_msg(NOTICE, "Package %s version %s has no "
				 "valid architecture, ignoring.\n",
				 pkg->name, version_str);
			free(version_str);
			continue;
		}

		if (cb)
			cb(pkg, priv);
		else
			hash_insert_pkg(pkg, is_status_file);

	} while (!feof(fp));

	free(buf);
	fclose(fp);

	if (src && src->gzip)
		gzip_close(&zh);

	return ret;
}

/*
 * Load in feed files from the cached "src" and/or "src/gz" locations.
 */
int pkg_hash_load_feeds(int state_flags, void (*cb)(pkg_t *, void *), void *priv)
{
	pkg_src_list_elt_t *iter;
	pkg_src_t *src;
	char *list_file, *lists_dir;

	opkg_msg(INFO, "\n");

	lists_dir = conf->restrict_to_default_dest ?
	    conf->default_dest->lists_dir : conf->lists_dir;

	for (iter = void_list_first(&conf->pkg_src_list); iter;
	     iter = void_list_next(&conf->pkg_src_list, iter)) {

		src = (pkg_src_t *) iter->data;

		list_file = concat_path_file(lists_dir, src->name);

		if (file_exists(list_file)) {
			if (pkg_hash_add_from_file(list_file, src, NULL, 0, state_flags, cb, priv)) {
				free(list_file);
				return -1;
			}
		}
		free(list_file);
	}

	return 0;
}

/*
 * Load in status files from the configured "dest"s.
 */
int pkg_hash_load_status_files(void (*cb)(pkg_t *, void *), void *priv)
{
	pkg_dest_list_elt_t *iter;
	pkg_dest_t *dest;

	opkg_msg(INFO, "\n");

	for (iter = void_list_first(&conf->pkg_dest_list); iter;
	     iter = void_list_next(&conf->pkg_dest_list, iter)) {

		dest = (pkg_dest_t *) iter->data;

		if (file_exists(dest->status_file_name)) {
			if (pkg_hash_add_from_file
			    (dest->status_file_name, NULL, dest, 1, SF_NEED_DETAIL, cb, priv))
				return -1;
		}
	}

	return 0;
}

static void
pkg_hash_load_package_details_helper(const char *pkg_name, void *entry, void *data)
{
	int *count = data;
	abstract_pkg_t *ab_pkg = (abstract_pkg_t *) entry;

	if (ab_pkg->state_flag & SF_NEED_DETAIL) {
		if (ab_pkg->state_flag & SF_MARKED) {
			opkg_msg(DEBUG, "skipping already seen flagged abpkg %s\n",
			         ab_pkg->name);
			return;
		}

		opkg_msg(DEBUG, "found yet incomplete flagged abpkg %s\n",
		         ab_pkg->name);

		(*count)++;
		ab_pkg->state_flag |= SF_MARKED;
	}
}

int pkg_hash_load_package_details(void)
{
	int n_need_detail;

	while (1) {
		pkg_hash_load_feeds(0, NULL, NULL);

		n_need_detail = 0;
		hash_table_foreach(&conf->pkg_hash, pkg_hash_load_package_details_helper, &n_need_detail);

		if (n_need_detail > 0)
			opkg_msg(DEBUG, "Found %d packages requiring details, reloading feeds\n", n_need_detail);
		else
			break;
	}

	return 0;
}

static int
pkg_hash_check_unresolved(pkg_t *maybe)
{
	char **unresolved = NULL;
	char **tmp;
	pkg_vec_t *depends;
	int res = 0;

	depends = pkg_vec_alloc();
	pkg_hash_fetch_unsatisfied_dependencies(maybe, depends, &unresolved, 1);

	if (unresolved) {
		res = 1;
		tmp = unresolved;
		while (*tmp) {
			opkg_msg(ERROR, "cannot find dependency %s for %s\n", *tmp, maybe->name);
			free(*(tmp++));
		}
		free(unresolved);
	}
	pkg_vec_free(depends);

	return res;
}

pkg_t *pkg_hash_fetch_best_installation_candidate(abstract_pkg_t * apkg,
						  int (*constraint_fcn) (pkg_t *
									 pkg,
									 void
									 *cdata),
						  void *cdata, int quiet)
{
	int i, j;
	int nprovides = 0;
	int nmatching = 0;
	int wrong_arch_found = 0;
	int arch_priority;
	int good_pkg_score = 0;
	pkg_vec_t *matching_pkgs;
	abstract_pkg_vec_t *matching_apkgs;
	abstract_pkg_vec_t *provided_apkg_vec;
	abstract_pkg_t **provided_apkgs;
	abstract_pkg_vec_t *providers;
	pkg_t *latest_installed_parent = NULL;
	pkg_t *latest_matching = NULL;
	pkg_t *priorized_matching = NULL;
	pkg_t *held_pkg = NULL;
	pkg_t *good_pkg_by_name = NULL;

	if (apkg == NULL || apkg->provided_by == NULL
	    || (apkg->provided_by->len == 0))
		return NULL;

	matching_pkgs = pkg_vec_alloc();
	matching_apkgs = abstract_pkg_vec_alloc();
	providers = abstract_pkg_vec_alloc();

	opkg_msg(DEBUG, "Best installation candidate for %s:\n", apkg->name);

	provided_apkg_vec = apkg->provided_by;
	nprovides = provided_apkg_vec->len;
	provided_apkgs = provided_apkg_vec->pkgs;
	if (nprovides > 1)
		opkg_msg(DEBUG, "apkg=%s nprovides=%d.\n", apkg->name,
			 nprovides);

	/* accumulate all the providers */
	for (i = 0; i < nprovides; i++) {
		abstract_pkg_t *provider_apkg = provided_apkgs[i];
		opkg_msg(DEBUG, "Adding %s to providers.\n",
			 provider_apkg->name);
		abstract_pkg_vec_insert(providers, provider_apkg);
	}
	nprovides = providers->len;

	for (i = 0; i < nprovides; i++) {
		abstract_pkg_t *provider_apkg =
		    abstract_pkg_vec_get(providers, i);
		abstract_pkg_t *replacement_apkg = NULL;
		pkg_vec_t *vec;

		if (provider_apkg->replaced_by
		    && provider_apkg->replaced_by->len) {
			replacement_apkg = provider_apkg->replaced_by->pkgs[0];
			if (provider_apkg->replaced_by->len > 1) {
				opkg_msg(NOTICE, "Multiple replacers for %s, "
					 "using first one (%s).\n",
					 provider_apkg->name,
					 replacement_apkg->name);
			}
		}

		if (replacement_apkg)
			opkg_msg(DEBUG,
				 "replacement_apkg=%s for provider_apkg=%s.\n",
				 replacement_apkg->name, provider_apkg->name);

		if (replacement_apkg && (replacement_apkg != provider_apkg)) {
			if (abstract_pkg_vec_contains
			    (providers, replacement_apkg))
				continue;
			else
				provider_apkg = replacement_apkg;
		}

		if (!(vec = provider_apkg->pkgs)) {
			opkg_msg(DEBUG, "No pkgs for provider_apkg %s.\n",
				 provider_apkg->name);
			continue;
		}

		/* now check for supported architecture */
		{
			int max_count = 0;

			/* count packages matching max arch priority and keep track of last one */
			for (j = 0; j < vec->len; j++) {
				pkg_t *maybe = vec->pkgs[j];
				arch_priority = pkg_get_arch_priority(maybe);

				opkg_msg(DEBUG,
					 "%s arch=%s arch_priority=%d version=%s.\n",
					 maybe->name, pkg_get_architecture(maybe),
					 arch_priority, pkg_get_string(maybe, PKG_VERSION));
				/* We make sure not to add the same package twice. Need to search for the reason why
				   they show up twice sometimes. */
				if ((arch_priority > 0)
				    &&
				    (!pkg_vec_contains(matching_pkgs, maybe))) {
					if (!pkg_hash_check_unresolved(maybe)) {
						max_count++;
						abstract_pkg_vec_insert(matching_apkgs,
									maybe->parent);
						pkg_vec_insert(matching_pkgs, maybe);
					}
				}
			}

			if (vec->len > 0 && matching_pkgs->len < 1)
				wrong_arch_found = 1;
		}
	}

	if (matching_pkgs->len < 1) {
		if (wrong_arch_found)
			opkg_msg(ERROR, "Packages for %s found, but"
				 " incompatible with the architectures configured\n",
				 apkg->name);
		pkg_vec_free(matching_pkgs);
		abstract_pkg_vec_free(matching_apkgs);
		abstract_pkg_vec_free(providers);
		return NULL;
	}

	if (matching_pkgs->len > 1)
		pkg_vec_sort(matching_pkgs,
			     pkg_name_version_and_architecture_compare);
	if (matching_apkgs->len > 1)
		abstract_pkg_vec_sort(matching_apkgs, abstract_pkg_name_compare);

	for (i = 0; i < matching_pkgs->len; i++) {
		pkg_t *matching = matching_pkgs->pkgs[i];
		if (constraint_fcn(matching, cdata)) {
			int score = 1;
			if (strcmp(matching->name, apkg->name) == 0)
				score++;

			for (j = 0; j < opkg_cli_argc; ++j) {
				if (!strcmp(matching->name, opkg_cli_argv[j])) {
					score++;
					break;
				}
			}

			opkg_msg(DEBUG, "Candidate: %s %s (score %d).\n",
				 matching->name, pkg_get_string(matching, PKG_VERSION),
				 score);
			if (score < good_pkg_score)
				continue;

			good_pkg_by_name = matching;
			good_pkg_score = score;
			/* It has been provided by hand, so it is what user want */
			if (matching->provided_by_hand == 1)
				break;
		}
	}

	for (i = 0; i < matching_pkgs->len; i++) {
		pkg_t *matching = matching_pkgs->pkgs[i];
		latest_matching = matching;
		if (matching->parent->state_status == SS_INSTALLED
		    || matching->parent->state_status == SS_UNPACKED)
			latest_installed_parent = matching;
		if (matching->state_flag & (SF_HOLD | SF_PREFER)) {
			if (held_pkg)
				opkg_msg(NOTICE,
					 "Multiple packages (%s and %s) providing"
					 " same name marked HOLD or PREFER. "
					 "Using latest.\n", held_pkg->name,
					 matching->name);
			held_pkg = matching;
		}
	}

	if (!good_pkg_by_name && !held_pkg && !latest_installed_parent
	    && matching_apkgs->len > 1 && !quiet) {
		int prio = 0;
		for (i = 0; i < matching_pkgs->len; i++) {
			pkg_t *matching = matching_pkgs->pkgs[i];
			arch_priority = pkg_get_arch_priority(matching);
			if (arch_priority > prio) {
				priorized_matching = matching;
				prio = arch_priority;
				opkg_msg(DEBUG, "Match %s with priority %i.\n",
					 matching->name, prio);
			}
		}

	}

	if (conf->verbosity >= INFO && matching_apkgs->len > 1) {
		opkg_msg(INFO, "%d matching pkgs for apkg=%s:\n",
			 matching_pkgs->len, apkg->name);
		for (i = 0; i < matching_pkgs->len; i++) {
			pkg_t *matching = matching_pkgs->pkgs[i];
			opkg_msg(INFO, "%s %s %s\n",
				 matching->name, pkg_get_string(matching, PKG_VERSION),
				 pkg_get_architecture(matching));
		}
	}

	nmatching = matching_apkgs->len;

	pkg_vec_free(matching_pkgs);
	abstract_pkg_vec_free(matching_apkgs);
	abstract_pkg_vec_free(providers);

	if (good_pkg_by_name) {	/* We found a good candidate, we will install it */
		return good_pkg_by_name;
	}
	if (held_pkg) {
		opkg_msg(INFO, "Using held package %s.\n", held_pkg->name);
		return held_pkg;
	}
	if (latest_installed_parent) {
		opkg_msg(INFO,
			 "Using latest version of installed package %s.\n",
			 latest_installed_parent->name);
		return latest_installed_parent;
	}
	if (priorized_matching) {
		opkg_msg(INFO, "Using priorized matching %s %s %s.\n",
			 priorized_matching->name, pkg_get_string(priorized_matching, PKG_VERSION),
			 pkg_get_architecture(priorized_matching));
		return priorized_matching;
	}
	if (nmatching > 1) {
		opkg_msg(INFO, "No matching pkg out of %d matching_apkgs.\n",
			 nmatching);
		return NULL;
	}
	if (latest_matching) {
		opkg_msg(INFO, "Using latest matching %s %s %s.\n",
			 latest_matching->name, pkg_get_string(latest_matching, PKG_VERSION),
			 pkg_get_architecture(latest_matching));
		return latest_matching;
	}
	return NULL;
}

static int pkg_name_constraint_fcn(pkg_t * pkg, void *cdata)
{
	const char *name = (const char *)cdata;

	if (strcmp(pkg->name, name) == 0)
		return 1;
	else
		return 0;
}

static pkg_vec_t *pkg_vec_fetch_by_name(const char *pkg_name)
{
	abstract_pkg_t *ab_pkg;

	if (!(ab_pkg = abstract_pkg_fetch_by_name(pkg_name)))
		return NULL;

	if (ab_pkg->pkgs)
		return ab_pkg->pkgs;

	if (ab_pkg->provided_by) {
		abstract_pkg_t *abpkg =
		    abstract_pkg_vec_get(ab_pkg->provided_by, 0);
		if (abpkg != NULL)
			return abpkg->pkgs;
		else
			return ab_pkg->pkgs;
	}

	return NULL;
}

pkg_t *pkg_hash_fetch_best_installation_candidate_by_name(const char *name)
{
	abstract_pkg_t *apkg = NULL;

	if (!(apkg = abstract_pkg_fetch_by_name(name)))
		return NULL;

	return pkg_hash_fetch_best_installation_candidate(apkg,
							  pkg_name_constraint_fcn,
							  apkg->name, 0);
}

pkg_t *pkg_hash_fetch_by_name_version(const char *pkg_name, const char *version)
{
	pkg_vec_t *vec;
	int i;
	char *version_str = NULL;

	if (!(vec = pkg_vec_fetch_by_name(pkg_name)))
		return NULL;

	for (i = 0; i < vec->len; i++) {
		version_str = pkg_version_str_alloc(vec->pkgs[i]);
		if (!strcmp(version_str, version)) {
			free(version_str);
			break;
		}
		free(version_str);
	}

	if (i == vec->len)
		return NULL;

	return vec->pkgs[i];
}

pkg_t *pkg_hash_fetch_installed_by_name_dest(const char *pkg_name,
					     pkg_dest_t * dest)
{
	pkg_vec_t *vec;
	int i;

	if (!(vec = pkg_vec_fetch_by_name(pkg_name))) {
		return NULL;
	}

	for (i = 0; i < vec->len; i++)
		if ((vec->pkgs[i]->state_status == SS_INSTALLED
		     || vec->pkgs[i]->state_status == SS_UNPACKED)
		    && vec->pkgs[i]->dest == dest) {
			return vec->pkgs[i];
		}

	return NULL;
}

pkg_t *pkg_hash_fetch_installed_by_name(const char *pkg_name)
{
	pkg_vec_t *vec;
	int i;

	if (!(vec = pkg_vec_fetch_by_name(pkg_name))) {
		return NULL;
	}

	for (i = 0; i < vec->len; i++) {
		if (vec->pkgs[i]->state_status == SS_INSTALLED
		    || vec->pkgs[i]->state_status == SS_UNPACKED) {
			return vec->pkgs[i];
		}
	}

	return NULL;
}

static void
pkg_hash_fetch_available_helper(const char *pkg_name, void *entry, void *data)
{
	int j;
	abstract_pkg_t *ab_pkg = (abstract_pkg_t *) entry;
	pkg_vec_t *all = (pkg_vec_t *) data;
	pkg_vec_t *pkg_vec = ab_pkg->pkgs;

	if (!pkg_vec)
		return;

	for (j = 0; j < pkg_vec->len; j++) {
		pkg_t *pkg = pkg_vec->pkgs[j];
		pkg_vec_insert(all, pkg);
	}
}

void pkg_hash_fetch_available(pkg_vec_t * all)
{
	hash_table_foreach(&conf->pkg_hash, pkg_hash_fetch_available_helper,
			   all);
}

static void
pkg_hash_fetch_all_installed_helper(const char *pkg_name, void *entry,
				    void *data)
{
	abstract_pkg_t *ab_pkg = (abstract_pkg_t *) entry;
	pkg_vec_t *all = (pkg_vec_t *) data;
	pkg_vec_t *pkg_vec = ab_pkg->pkgs;
	int j;

	if (!pkg_vec)
		return;

	for (j = 0; j < pkg_vec->len; j++) {
		pkg_t *pkg = pkg_vec->pkgs[j];
		if (pkg->state_status == SS_INSTALLED
		    || pkg->state_status == SS_UNPACKED)
			pkg_vec_insert(all, pkg);
	}
}

void pkg_hash_fetch_all_installed(pkg_vec_t * all)
{
	hash_table_foreach(&conf->pkg_hash, pkg_hash_fetch_all_installed_helper,
			   all);
}

/*
 * This assumes that the abstract pkg doesn't exist.
 */
static abstract_pkg_t *add_new_abstract_pkg_by_name(const char *pkg_name)
{
	abstract_pkg_t *ab_pkg;

	ab_pkg = abstract_pkg_new();

	ab_pkg->name = xstrdup(pkg_name);
	hash_table_insert(&conf->pkg_hash, pkg_name, ab_pkg);

	return ab_pkg;
}

abstract_pkg_t *ensure_abstract_pkg_by_name(const char *pkg_name)
{
	abstract_pkg_t *ab_pkg;

	if (!(ab_pkg = abstract_pkg_fetch_by_name(pkg_name)))
		ab_pkg = add_new_abstract_pkg_by_name(pkg_name);

	return ab_pkg;
}

void hash_insert_pkg(pkg_t * pkg, int set_status)
{
	abstract_pkg_t *ab_pkg;

	ab_pkg = ensure_abstract_pkg_by_name(pkg->name);
	if (!ab_pkg->pkgs)
		ab_pkg->pkgs = pkg_vec_alloc();

	if (pkg->state_status == SS_INSTALLED) {
		ab_pkg->state_status = SS_INSTALLED;
	} else if (pkg->state_status == SS_UNPACKED) {
		ab_pkg->state_status = SS_UNPACKED;
	}

	init_providelist(pkg, NULL);

	buildDependedUponBy(pkg, ab_pkg);

	pkg_vec_insert_merge(ab_pkg->pkgs, pkg, set_status);
	pkg->parent = ab_pkg;
}

static const char *strip_offline_root(const char *file_name)
{
	unsigned int len;

	if (conf->offline_root) {
		len = strlen(conf->offline_root);
		if (strncmp(file_name, conf->offline_root, len) == 0)
			file_name += len;
	}

	return file_name;
}

void file_hash_remove(const char *file_name)
{
	file_name = strip_offline_root(file_name);
	hash_table_remove(&conf->file_hash, file_name);
}

pkg_t *file_hash_get_file_owner(const char *file_name)
{
	file_name = strip_offline_root(file_name);
	return hash_table_get(&conf->file_hash, file_name);
}

void file_hash_set_file_owner(const char *file_name, pkg_t * owning_pkg)
{
	pkg_t *old_owning_pkg;
	int file_name_len = strlen(file_name);

	if (file_name[file_name_len - 1] == '/')
		return;

	file_name = strip_offline_root(file_name);

	old_owning_pkg = hash_table_get(&conf->file_hash, file_name);
	hash_table_insert(&conf->file_hash, file_name, owning_pkg);

	if (old_owning_pkg) {
		pkg_get_installed_files(old_owning_pkg);
		str_list_remove_elt(old_owning_pkg->installed_files, file_name);
		pkg_free_installed_files(old_owning_pkg);

		/* mark this package to have its filelist written */
		old_owning_pkg->state_flag |= SF_FILELIST_CHANGED;
		owning_pkg->state_flag |= SF_FILELIST_CHANGED;
	}
}
