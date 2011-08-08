#include <archive_entry.h>
#include <string.h>

#include "pkg.h"
#include "pkg_private.h"
#include "pkg_event.h"

int
pkgdb_dump(struct pkgdb *db, char *dest)
{
	struct pkgdb_it *it;
	struct pkg *pkg = NULL;
	struct sbuf *path;
	struct packing *pack;
	char *m;
	int ret;
	int query_flags = PKG_LOAD_DEPS | PKG_LOAD_CONFLICTS | PKG_LOAD_FILES | PKG_LOAD_CATEGORIES |
					  PKG_LOAD_DIRS | PKG_LOAD_SCRIPTS | PKG_LOAD_OPTIONS |
					  PKG_LOAD_MTREE | PKG_LOAD_LICENSES;

	packing_init(&pack, dest ? dest : "./pkgdump", TXZ);

	path = sbuf_new_auto();
	if ((it = pkgdb_query(db, NULL, MATCH_ALL)) == NULL) {
		/* TODO handle errors */
		return (EPKG_FATAL);
	}

	while ((ret = pkgdb_it_next(it, &pkg, query_flags)) == EPKG_OK) {
		pkg_emit_manifest(pkg, &m);
		sbuf_clear(path);
		sbuf_printf(path, "%s-%s.yaml", pkg_get(pkg, PKG_NAME), pkg_get(pkg, PKG_VERSION));
		packing_append_buffer(pack, m, sbuf_data(path), strlen(m));
		free(m);
		if (pkg_get(pkg, PKG_MTREE) != NULL) {
			sbuf_clear(path);
			sbuf_printf(path, "%s-%s.mtree", pkg_get(pkg, PKG_NAME), pkg_get(pkg, PKG_VERSION));
			packing_append_buffer(pack, pkg_get(pkg, PKG_MTREE), sbuf_data(path), strlen(pkg_get(pkg, PKG_MTREE)));
		}
	}

	sbuf_delete(path);
	packing_finish(pack);
	return (EPKG_OK);
}

int
pkgdb_load(struct pkgdb *db, char *dest)
{
	struct pkg *pkg = NULL;
	struct archive *a;
	struct archive_entry *ae;
	const char *path;
	size_t len;
	char *buf;
	size_t size;
	int retcode = EPKG_OK;

	a = archive_read_new();
	archive_read_support_compression_all(a);
	archive_read_support_format_tar(a);

	if (archive_read_open_filename(a, dest, 4096) != ARCHIVE_OK) {
		EMIT_PKG_ERROR("archiv_read_open_filename(%s): %s", path, archive_error_string(a));
		retcode = EPKG_FATAL;
		goto cleanup;
	}

	while (archive_read_next_header(a, &ae) == ARCHIVE_OK) {
		path = archive_entry_pathname(ae);
		len = strlen(path);
		if (len < 6)
			continue;
		if (!strcmp(path + len - 5, ".yaml")) {
			if (pkg == NULL) {
				pkg_new(&pkg, PKG_FILE);
			} else {
				pkgdb_register_finale(db, pkgdb_register_pkg(db, pkg ));
				pkg_reset(pkg, PKG_FILE);
			}
			size = archive_entry_size(ae);
			buf = calloc(1, size + 1);
			archive_read_data(a, buf, size);
			pkg_parse_manifest(pkg, buf);
			free(buf);
		} else if (!strcmp(path + len - 6, ".mtree")) {
			size = archive_entry_size(ae);
			buf = calloc(1, size + 1);
			archive_read_data(a, buf, size);
			pkg_set(pkg, PKG_MTREE, buf);
			free(buf);
		} else 
			continue;
	}

cleanup:
	if (a != NULL)
		archive_read_finish(a);
	pkgdb_close(db);
	pkg_free(pkg);

	return (retcode);
}