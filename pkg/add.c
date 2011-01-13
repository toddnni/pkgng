#include <stdio.h>
#include <pkg.h>
#include <stdbool.h>
#include <err.h>

#include "add.h"

void
usage_add(void)
{
	fprintf(stderr, "add ... <pkg-name>\n"
			"add\n");
}

int
exec_add(int argc, char **argv)
{
	struct pkgdb *db;
	struct pkgdb_it *it;
	struct pkg *pkg;
	struct pkg *p;
	struct pkg **deps;
	int ret = 0;

	bool installed = false;

	int numdeps, i;

	if (argc != 2)
		return (-1);

	if (pkg_open(argv[1], &pkg, 0) != 0) {
		return (-1);
	}

	if (pkgdb_open(&db) == -1) {
		pkgdb_warn(db);
		return (-1);
	}

	/* check if already installed */
	if ((it = pkgdb_query(db, pkg_name(pkg), MATCH_EXACT)) == NULL) {
		pkgdb_warn(db);
		return (-1);
	}

	pkg_new(&p);

	if (pkgdb_it_next_pkg(it, &p, PKG_BASIC) == 0) {
		installed = true;
	}
	pkgdb_it_free(it);


	if (installed) {
		err(1, "%s is already installed\n", pkg_name(pkg));
	}


	if ((numdeps = pkg_numdeps(pkg)) > 0) {
		pkg_resolvdeps(pkg, db);

		deps = pkg_deps(pkg);
		for (i = 0; deps[i] != NULL; i++) {
			if (pkg_type(deps[i]) == PKG_NOTFOUND) {
				warnx("%s-%s: unresolved dependency %s-%s", pkg_name(pkg), pkg_version(pkg), pkg_name(deps[i]), pkg_version(deps[i]));
				ret = 1;
			}
		}
	}

	if (ret != 0)
		return (ret);

	pkgdb_close(db);

	return (0);
}