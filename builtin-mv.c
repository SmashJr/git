/*
 * "git mv" builtin command
 *
 * Copyright (C) 2006 Johannes Schindelin
 */
#include <fnmatch.h>

#include "cache.h"
#include "builtin.h"
#include "dir.h"
#include "cache-tree.h"
#include "path-list.h"

static const char builtin_mv_usage[] =
"git-mv [-n] [-f] (<source> <destination> | [-k] <source>... <destination>)";

static const char **copy_pathspec(const char *prefix, const char **pathspec,
				  int count, int base_name)
{
	const char **result = xmalloc((count + 1) * sizeof(const char *));
	memcpy(result, pathspec, count * sizeof(const char *));
	result[count] = NULL;
	if (base_name) {
		int i;
		for (i = 0; i < count; i++) {
			const char *last_slash = strrchr(result[i], '/');
			if (last_slash)
				result[i] = last_slash + 1;
		}
	}
	return get_pathspec(prefix, result);
}

static void show_list(const char *label, struct path_list *list)
{
	if (list->nr > 0) {
		int i;
		printf("%s", label);
		for (i = 0; i < list->nr; i++)
			printf("%s%s", i > 0 ? ", " : "", list->items[i].path);
		putchar('\n');
	}
}

static const char *add_slash(const char *path)
{
	int len = strlen(path);
	if (path[len - 1] != '/') {
		char *with_slash = xmalloc(len + 2);
		memcpy(with_slash, path, len);
		with_slash[len++] = '/';
		with_slash[len] = 0;
		return with_slash;
	}
	return path;
}

static struct lock_file lock_file;

int cmd_mv(int argc, const char **argv, const char *prefix)
{
	int i, newfd, count;
	int verbose = 0, show_only = 0, force = 0, ignore_errors = 0;
	const char **source, **destination, **dest_path;
	enum update_mode { BOTH = 0, WORKING_DIRECTORY, INDEX } *modes;
	struct stat st;
	struct path_list overwritten = {NULL, 0, 0, 0};
	struct path_list src_for_dst = {NULL, 0, 0, 0};
	struct path_list added = {NULL, 0, 0, 0};
	struct path_list deleted = {NULL, 0, 0, 0};
	struct path_list changed = {NULL, 0, 0, 0};

	git_config(git_default_config);

	newfd = hold_lock_file_for_update(&lock_file, get_index_file(), 1);
	if (read_cache() < 0)
		die("index file corrupt");

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (arg[0] != '-')
			break;
		if (!strcmp(arg, "--")) {
			i++;
			break;
		}
		if (!strcmp(arg, "-n")) {
			show_only = 1;
			continue;
		}
		if (!strcmp(arg, "-f")) {
			force = 1;
			continue;
		}
		if (!strcmp(arg, "-k")) {
			ignore_errors = 1;
			continue;
		}
		usage(builtin_mv_usage);
	}
	count = argc - i - 1;
	if (count < 1)
		usage(builtin_mv_usage);

	source = copy_pathspec(prefix, argv + i, count, 0);
	modes = xcalloc(count, sizeof(enum update_mode));
	dest_path = copy_pathspec(prefix, argv + argc - 1, 1, 0);

	if (!lstat(dest_path[0], &st) &&
			S_ISDIR(st.st_mode)) {
		dest_path[0] = add_slash(dest_path[0]);
		destination = copy_pathspec(dest_path[0], argv + i, count, 1);
	} else {
		if (count != 1)
			usage(builtin_mv_usage);
		destination = dest_path;
	}

	/* Checking */
	for (i = 0; i < count; i++) {
		int length;
		const char *bad = NULL;

		if (show_only)
			printf("Checking rename of '%s' to '%s'\n",
				source[i], destination[i]);

		if (lstat(source[i], &st) < 0)
			bad = "bad source";

		if (S_ISDIR(st.st_mode)) {
			const char *dir = source[i], *dest_dir = destination[i];
			int first, last, len = strlen(dir);

			if (lstat(dest_dir, &st) == 0) {
				bad = "cannot move directory over file";
				goto next;
			}

			modes[i] = WORKING_DIRECTORY;

			first = cache_name_pos(source[i], len);
			if (first >= 0)
				die ("Huh? %s/ is in index?", dir);

			first = -1 - first;
			for (last = first; last < active_nr; last++) {
				const char *path = active_cache[last]->name;
				if (strncmp(path, dir, len) || path[len] != '/')
					break;
			}

			if (last - first < 1)
				bad = "source directory is empty";
			else if (!bad) {
				int j, dst_len = strlen(dest_dir);

				if (last - first > 0) {
					source = realloc(source,
							(count + last - first)
							* sizeof(char *));
					destination = realloc(destination,
							(count + last - first)
							* sizeof(char *));
					modes = realloc(modes,
							(count + last - first)
							* sizeof(enum update_mode));
				}

				dest_dir = add_slash(dest_dir);

				for (j = 0; j < last - first; j++) {
					const char *path =
						active_cache[first + j]->name;
					source[count + j] = path;
					destination[count + j] =
						prefix_path(dest_dir, dst_len,
							path + len);
					modes[count + j] = INDEX;
				}
				count += last - first;
			}

			goto next;
		}

		if (!bad && lstat(destination[i], &st) == 0) {
			bad = "destination exists";
			if (force) {
				/*
				 * only files can overwrite each other:
				 * check both source and destination
				 */
				if (S_ISREG(st.st_mode)) {
					fprintf(stderr, "Warning: %s;"
							" will overwrite!\n",
							bad);
					bad = NULL;
					path_list_insert(destination[i],
							&overwritten);
				} else
					bad = "Cannot overwrite";
			}
		}

		if (!bad &&
		    (length = strlen(source[i])) >= 0 &&
		    !strncmp(destination[i], source[i], length) &&
		    (destination[i][length] == 0 || destination[i][length] == '/'))
			bad = "can not move directory into itself";

		if (!bad && cache_name_pos(source[i], strlen(source[i])) < 0)
			bad = "not under version control";

		if (!bad) {
			if (path_list_has_path(&src_for_dst, destination[i]))
				bad = "multiple sources for the same target";
			else
				path_list_insert(destination[i], &src_for_dst);
		}

next:
		if (bad) {
			if (ignore_errors) {
				if (--count > 0) {
					memmove(source + i, source + i + 1,
						(count - i) * sizeof(char *));
					memmove(destination + i,
						destination + i + 1,
						(count - i) * sizeof(char *));
				}
			} else
				die ("%s, source=%s, destination=%s",
				     bad, source[i], destination[i]);
		}
	}

	for (i = 0; i < count; i++) {
		if (show_only || verbose)
			printf("Renaming %s to %s\n",
			       source[i], destination[i]);
		if (!show_only && modes[i] != INDEX &&
		    rename(source[i], destination[i]) < 0 &&
		    !ignore_errors)
			die ("renaming %s failed: %s",
			     source[i], strerror(errno));

		if (modes[i] == WORKING_DIRECTORY)
			continue;

		if (cache_name_pos(source[i], strlen(source[i])) >= 0) {
			path_list_insert(source[i], &deleted);

			/* destination can be a directory with 1 file inside */
			if (path_list_has_path(&overwritten, destination[i]))
				path_list_insert(destination[i], &changed);
			else
				path_list_insert(destination[i], &added);
		} else
			path_list_insert(destination[i], &added);
	}

        if (show_only) {
		show_list("Changed  : ", &changed);
		show_list("Adding   : ", &added);
		show_list("Deleting : ", &deleted);
	} else {
		for (i = 0; i < changed.nr; i++) {
			const char *path = changed.items[i].path;
			int i = cache_name_pos(path, strlen(path));
			struct cache_entry *ce = active_cache[i];

			if (i < 0)
				die ("Huh? Cache entry for %s unknown?", path);
			refresh_cache_entry(ce, 0);
		}

		for (i = 0; i < added.nr; i++) {
			const char *path = added.items[i].path;
			add_file_to_index(path, verbose);
		}

		for (i = 0; i < deleted.nr; i++) {
			const char *path = deleted.items[i].path;
			remove_file_from_cache(path);
		}

		if (active_cache_changed) {
			if (write_cache(newfd, active_cache, active_nr) ||
			    close(newfd) ||
			    commit_lock_file(&lock_file))
				die("Unable to write new index file");
		}
	}

	return 0;
}
