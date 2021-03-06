#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <grp.h>
#include <pwd.h>
#include <libutil.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include <sqlite3.h>

#include "pkg.h"
#include "pkg_event.h"
#include "pkg_private.h"
#include "pkgdb.h"
#include "pkg_util.h"

#include "db_upgrades.h"
#define DBVERSION 8

static struct pkgdb_it * pkgdb_it_new(struct pkgdb *, sqlite3_stmt *, int);
static void pkgdb_regex(sqlite3_context *, int, sqlite3_value **, int);
static void pkgdb_regex_basic(sqlite3_context *, int, sqlite3_value **);
static void pkgdb_regex_extended(sqlite3_context *, int, sqlite3_value **);
static void pkgdb_regex_delete(void *);
static void pkgdb_pkglt(sqlite3_context *, int, sqlite3_value **);
static void pkgdb_pkggt(sqlite3_context *, int, sqlite3_value **);
static int get_pragma(sqlite3 *, const char *, int64_t *);
static int pkgdb_upgrade(struct pkgdb *);
static void populate_pkg(sqlite3_stmt *stmt, struct pkg *pkg);
static int create_temporary_pkgjobs(sqlite3 *);
static void pkgdb_detach_remotes(sqlite3 *);
static bool is_attached(sqlite3 *, const char *);

static struct column_mapping {
	const char * const name;
	pkg_attr type;
} columns[] = {
	{ "origin", PKG_ORIGIN},
	{ "name", PKG_NAME },
	{ "version", PKG_VERSION },
	{ "comment", PKG_COMMENT },
	{ "desc", PKG_DESC },
	{ "message", PKG_MESSAGE },
	{ "arch", PKG_ARCH },
	{ "osversion", PKG_OSVERSION},
	{ "maintainer", PKG_MAINTAINER},
	{ "www", PKG_WWW},
	{ "prefix", PKG_PREFIX},
	{ "cksum", PKG_CKSUM},
	{ "repopath", PKG_REPOPATH},
	{ "dbname", PKG_REPONAME},
	{ "newversion", PKG_NEWVERSION},
	{ "flatsize", PKG_FLATSIZE },
	{ "newflatsize", PKG_NEW_FLATSIZE },
	{ "pkgsize", PKG_NEW_PKGSIZE },
	{ "licenselogic", PKG_LICENSE_LOGIC},
	{ "automatic", PKG_AUTOMATIC},
	{ "rowid", PKG_ROWID},
	{ "id", PKG_ROWID },
	{ "weight", -1 },
	{ NULL, -1 }
};


static int
load_val(sqlite3 *db, struct pkg *pkg, const char *sql, int flags, int (*pkg_adddata)(struct pkg *pkg, const char *data), int list)
{
	sqlite3_stmt *stmt;
	int ret;

	assert(db != NULL && pkg != NULL);

	if (pkg->flags & flags)
		return (EPKG_OK);

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db);
		return (EPKG_FATAL);
	}

	sqlite3_bind_int64(stmt, 1, pkg->rowid);

	while (( ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		pkg_adddata(pkg, sqlite3_column_text(stmt, 0));
	}

	sqlite3_finalize(stmt);

	if (ret != SQLITE_DONE) {
		if (list != -1)
			pkg_list_free(pkg, list);
		ERROR_SQLITE(db);
		return (EPKG_FATAL);
	}

	pkg->flags |= flags;
	return (EPKG_OK);
}

static void
populate_pkg(sqlite3_stmt *stmt, struct pkg *pkg) {
	int i, icol = 0;
	const char *colname;

	assert(stmt != NULL);

	for (icol = 0; icol < sqlite3_column_count(stmt); icol++) {
		colname = sqlite3_column_name(stmt, icol);
		switch (sqlite3_column_type(stmt, icol)) {
			case SQLITE_TEXT:
				for (i = 0; columns[i].name != NULL; i++ ) {
					if (!strcmp(columns[i].name, colname)) {
						pkg_set(pkg, columns[i].type, sqlite3_column_text(stmt, icol));
						break;
					}
				}
				if (columns[i].name == NULL)
					pkg_emit_error("Unknown column %s", colname);
				break;
			case SQLITE_INTEGER:
				for (i = 0; columns[i].name != NULL; i++ ) {
					if (!strcmp(columns[i].name, colname)) {
						pkg_set(pkg, columns[i].type, sqlite3_column_int64(stmt, icol));
						break;
					}
				}
				if (columns[i].name == NULL)
					pkg_emit_error("Unknown column %s", colname);
				break;
			case SQLITE_BLOB:
			case SQLITE_FLOAT:
				pkg_emit_error("Wrong type for column: %s", colname);
				/* just ignore currently */
				break;
			case SQLITE_NULL:
				break;
		}
	}
}

static void
pkgdb_regex(sqlite3_context *ctx, int argc, sqlite3_value **argv, int reg_type)
{
	const unsigned char *regex = NULL;
	const unsigned char *str;
	regex_t *re;
	int ret;

	if (argc != 2 || (regex = sqlite3_value_text(argv[0])) == NULL ||
		(str = sqlite3_value_text(argv[1])) == NULL) {
		sqlite3_result_error(ctx, "SQL function regex() called with invalid arguments.\n", -1);
		return;
	}

	re = (regex_t *)sqlite3_get_auxdata(ctx, 0);
	if (re == NULL) {
		re = malloc(sizeof(regex_t));
		if (regcomp(re, regex, reg_type | REG_NOSUB) != 0) {
			sqlite3_result_error(ctx, "Invalid regex\n", -1);
			free(re);
			return;
		}

		sqlite3_set_auxdata(ctx, 0, re, pkgdb_regex_delete);
	}

	ret = regexec(re, str, 0, NULL, 0);
	sqlite3_result_int(ctx, (ret != REG_NOMATCH));
}

static void
pkgdb_regex_basic(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	pkgdb_regex(ctx, argc, argv, REG_BASIC);
}

static void
pkgdb_regex_extended(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	pkgdb_regex(ctx, argc, argv, REG_EXTENDED);
}

static void
pkgdb_regex_delete(void *p)
{
	regex_t *re = (regex_t *)p;

	regfree(re);
	free(re);
}

static void
pkgdb_pkgcmp(sqlite3_context *ctx, int argc, sqlite3_value **argv, int sign)
{
	const unsigned char *version1 = NULL;
	const unsigned char *version2 = NULL;
	if (argc != 2 || (version1 = sqlite3_value_text(argv[0])) == NULL
			|| (version2 = sqlite3_value_text(argv[1])) == NULL) {
		sqlite3_result_error(ctx, "Invalid comparison\n", -1);
		return;
	}

	sqlite3_result_int(ctx, (pkg_version_cmp(version1, version2) == sign));
}

static void
pkgdb_pkglt(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	pkgdb_pkgcmp(ctx, argc, argv, -1);
}

static void
pkgdb_pkggt(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	pkgdb_pkgcmp(ctx, argc, argv, 1);
}

static int
pkgdb_upgrade(struct pkgdb *db)
{
	int64_t db_version = -1;
	const char *sql_upgrade;
	int i;

	assert(db != NULL);

	if (get_pragma(db->sqlite, "PRAGMA user_version;", &db_version) != EPKG_OK)
		return (EPKG_FATAL);

	if (db_version == DBVERSION)
		return (EPKG_OK);
	else if (db_version > DBVERSION) {
		pkg_emit_error("database version is newer than libpkg(3)");
		return (EPKG_FATAL);
	}

	while (db_version < DBVERSION) {
		if (db->writable != 1) {
			pkg_emit_error("The database is outdated and opened readonly");
			return (EPKG_FATAL);
		}
		db_version++;

		i = 0;
		sql_upgrade = NULL;
		while (db_upgrades[i].version != -1) {
			if (db_upgrades[i].version == db_version) {
				sql_upgrade = db_upgrades[i].sql;
				break;
			}
			i++;
		}

		/*
		 * We can't find the statements to upgrade to the next version,
		 * maybe because the current version is too old and upgrade support has
		 * been removed.
		 */
		if (sql_upgrade == NULL) {
			pkg_emit_error("can not upgrade to db version %" PRId64,
						   db_version);
			return (EPKG_FATAL);
		}

		if (sql_exec(db->sqlite, "BEGIN;") != EPKG_OK)
			return (EPKG_FATAL);

		if (sql_exec(db->sqlite, sql_upgrade) != EPKG_OK)
			return (EPKG_FATAL);

		if (sql_exec(db->sqlite, "PRAGMA user_version = %" PRId64 ";", db_version) != EPKG_OK)
			return (EPKG_FATAL);

		if (sql_exec(db->sqlite, "COMMIT;") != EPKG_OK)
			return (EPKG_OK);
	}

	return (EPKG_OK);
}

/*
 * in the database : 
 * scripts.type can be:
 * - 0: PRE_INSTALL
 * - 1: POST_INSTALL
 * - 2: PRE_DEINSTALL
 * - 3: POST_DEINSTALL
 * - 4: PRE_UPGRADE
 * - 5: POST_UPGRADE
 * - 6: INSTALL
 * - 7: DEINSTALL
 * - 8: UPGRADE
 */

static int
pkgdb_init(sqlite3 *sdb)
{
	const char sql[] = ""
	"BEGIN;"
	"CREATE TABLE packages ("
		"id INTEGER PRIMARY KEY,"
		"origin TEXT UNIQUE NOT NULL,"
		"name TEXT NOT NULL,"
		"version TEXT NOT NULL,"
		"comment TEXT NOT NULL,"
		"desc TEXT NOT NULL,"
		"mtree_id INTEGER REFERENCES mtree(id) ON DELETE RESTRICT"
			" ON UPDATE CASCADE,"
		"message TEXT,"
		"arch TEXT NOT NULL,"
		"osversion TEXT NOT NULL,"
		"maintainer TEXT NOT NULL,"
		"www TEXT,"
		"prefix TEXT NOT NULL,"
		"flatsize INTEGER NOT NULL,"
		"automatic INTEGER NOT NULL,"
		"licenselogic INTEGER NOT NULL,"
		"pkg_format_version INTEGER"
	");"
	"CREATE TABLE mtree ("
		"id INTEGER PRIMARY KEY,"
		"content TEXT UNIQUE"
	");"
	"CREATE TABLE scripts ("
		"package_id INTEGER REFERENCES packages(id) ON DELETE CASCADE"
			" ON UPDATE CASCADE,"
		"script TEXT,"
		"type INTEGER,"
		"PRIMARY KEY (package_id, type)"
	");"
	"CREATE TABLE options ("
		"package_id INTEGER REFERENCES packages(id) ON DELETE CASCADE"
			" ON UPDATE CASCADE,"
		"option TEXT,"
		"value TEXT,"
		"PRIMARY KEY (package_id,option)"
	");"
	"CREATE TABLE deps ("
		"origin TEXT NOT NULL,"
		"name TEXT NOT NULL,"
		"version TEXT NOT NULL,"
		"package_id INTEGER REFERENCES packages(id) ON DELETE CASCADE"
			" ON UPDATE CASCADE,"
		"PRIMARY KEY (package_id,origin)"
	");"
	"CREATE TABLE files ("
		"path TEXT PRIMARY KEY,"
		"sha256 TEXT,"
		"package_id INTEGER REFERENCES packages(id) ON DELETE CASCADE"
			" ON UPDATE CASCADE"
	");"
	"CREATE TABLE directories ("
		"id INTEGER PRIMARY KEY,"
		"path TEXT NOT NULL UNIQUE"
	");"
	"CREATE TABLE pkg_directories ("
		"package_id INTEGER REFERENCES packages(id) ON DELETE CASCADE"
			" ON UPDATE CASCADE,"
		"directory_id INTEGER REFERENCES directories(id) ON DELETE RESTRICT"
			" ON UPDATE RESTRICT,"
		"try INTEGER,"
		"PRIMARY KEY (package_id, directory_id)"
	");"
	"CREATE TABLE categories ("
		"id INTEGER PRIMARY KEY,"
		"name TEXT NOT NULL UNIQUE"
	");"
	"CREATE TABLE pkg_categories ("
		"package_id INTEGER REFERENCES packages(id) ON DELETE CASCADE"
			" ON UPDATE CASCADE,"
		"category_id INTEGER REFERENCES categories(id) ON DELETE RESTRICT"
			" ON UPDATE RESTRICT,"
		"PRIMARY KEY (package_id, category_id)"
	");"
	"CREATE TABLE licenses ("
		"id INTEGER PRIMARY KEY,"
		"name TEXT NOT NULL UNIQUE"
	");"
	"CREATE TABLE pkg_licenses ("
		"package_id INTEGER REFERENCES packages(id) ON DELETE CASCADE"
			" ON UPDATE CASCADE,"
		"license_id INTEGER REFERENCES licenses(id) ON DELETE RESTRICT"
			" ON UPDATE RESTRICT,"
		"PRIMARY KEY (package_id, license_id)"
	");"
	"CREATE TABLE users ("
		"id INTEGER PRIMARY KEY,"
		"name TEXT NOT NULL UNIQUE"
	");"
	"CREATE TABLE pkg_users ("
		"package_id INTEGER REFERENCES packages(id) ON DELETE CASCADE"
			" ON UPDATE CASCADE,"
		"user_id INTEGER REFERENCES users(id) ON DELETE RESTRICT"
			" ON UPDATE RESTRICT,"
		"UNIQUE(package_id, user_id)"
	");"
	"CREATE TABLE groups ("
		"id INTEGER PRIMARY KEY,"
		"name TEXT NOT NULL UNIQUE"
	");"
	"CREATE TABLE pkg_groups ("
		"package_id INTEGER REFERENCES packages(id) ON DELETE CASCADE"
			" ON UPDATE CASCADE,"
		"group_id INTEGER REFERENCES groups(id) ON DELETE RESTRICT"
			" ON UPDATE RESTRICT,"
		"UNIQUE(package_id, group_id)"
	");"
	"CREATE INDEX deporigini on deps(origin);"
	"PRAGMA user_version = 8;"
	"COMMIT;"
	;

	return (sql_exec(sdb, sql));
}

int
pkgdb_open(struct pkgdb **db_p, pkgdb_t type)
{
	struct pkgdb *db = NULL;
	char localpath[MAXPATHLEN + 1];
	char remotepath[MAXPATHLEN + 1];
	const char *dbdir = NULL;
	const char *repo_name = NULL;
	bool multirepos_enabled = false;
	bool create = false;
	struct pkg_config_kv *repokv = NULL;

	/*
	 * Set the pointer to NULL now. Change it to the real pointer just
	 * before returning, when we know that we succeeded.
	 */
	*db_p = NULL;

	if (pkg_config_string(PKG_CONFIG_DBDIR, &dbdir) != EPKG_OK)
		return (EPKG_FATAL);

	if ((db = calloc(1, sizeof(struct pkgdb))) == NULL) {
		pkg_emit_errno("malloc", "pkgdb");
		return EPKG_FATAL;
	}

	db->type = type;

	snprintf(localpath, sizeof(localpath), "%s/local.sqlite", dbdir);

	if (eaccess(localpath, R_OK) != 0) {
		if (errno != ENOENT) {
			pkg_emit_errno("Unable to create local database ", localpath);
			pkgdb_close(db);
			return (EPKG_FATAL);
		} else if (eaccess(dbdir, W_OK) != 0) {
			/* If we need to create the db but can not write to it, fail early */
			pkg_emit_errno("Unable to create local database ", dbdir);
			pkgdb_close(db);
			return (EPKG_FATAL);
		} else {
			create = true;
		}
	}
	
	sqlite3_initialize();
	if (sqlite3_open(localpath, &db->sqlite) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		pkgdb_close(db);
		return (EPKG_FATAL);
	}

	/* If the database is missing we have to initialize it */
	if (create == true)
		if (pkgdb_init(db->sqlite) != EPKG_OK) {
			pkgdb_close(db);
			return (EPKG_FATAL);
		}

	if (eaccess(localpath, W_OK) == 0)
		db->writable = 1;

	if (pkgdb_upgrade(db) != EPKG_OK) {
		pkgdb_close(db);
		return (EPKG_FATAL);
	}

	sqlite3_create_function(db->sqlite, "regexp", 2, SQLITE_ANY, NULL,
							pkgdb_regex_basic, NULL, NULL);
	sqlite3_create_function(db->sqlite, "eregexp", 2, SQLITE_ANY, NULL,
							pkgdb_regex_extended, NULL, NULL);
	sqlite3_create_function(db->sqlite, "pkglt", 2, SQLITE_ANY, NULL,
			pkgdb_pkglt, NULL, NULL);
	sqlite3_create_function(db->sqlite, "pkggt", 2, SQLITE_ANY, NULL,
			pkgdb_pkggt, NULL, NULL);

	/*
	 * allow foreign key option which will allow to have clean support for
	 * reinstalling
	 */
	if (sql_exec(db->sqlite, "PRAGMA foreign_keys = ON;") != EPKG_OK) {
		pkgdb_close(db);
		return (EPKG_FATAL);
	}

	if (type == PKGDB_REMOTE) {
		pkg_config_bool(PKG_CONFIG_MULTIREPOS, &multirepos_enabled);

		if (multirepos_enabled) {
			fprintf(stderr, "\t/!\\		   WARNING WARNING WARNING		/!\\\n");
			fprintf(stderr, "\t/!\\	     WORKING ON MULTIPLE REPOSITORIES		/!\\\n");
			fprintf(stderr, "\t/!\\  THIS FEATURE IS STILL CONSIDERED EXPERIMENTAL	/!\\\n");
			fprintf(stderr, "\t/!\\		     YOU HAVE BEEN WARNED		/!\\\n\n");

			while (pkg_config_list(PKG_CONFIG_REPOS, &repokv) == EPKG_OK) {
				repo_name = pkg_config_kv_get(repokv, PKG_CONFIG_KV_KEY);

				/* is it a reserved name? */
				if ((strcmp(repo_name, "repo") == 0) ||
				    (strcmp(repo_name, "main") == 0) ||
				    (strcmp(repo_name, "temp") == 0) ||
				    (strcmp(repo_name, "local") == 0))
					continue;

				/* is it already attached? */
				if (is_attached(db->sqlite, repo_name)) {
					pkg_emit_error("repository '%s' is already listed, ignoring", repo_name);
					continue;
				}

				snprintf(remotepath, sizeof(remotepath), "%s/%s.sqlite",
						dbdir, repo_name);

				if (access(remotepath, R_OK) != 0) {
					pkg_emit_errno("access", remotepath);
					pkgdb_close(db);
					return (EPKG_FATAL);
				}
				
				if (sql_exec(db->sqlite, "ATTACH '%q' AS '%q';", remotepath, repo_name) != EPKG_OK) {
					pkgdb_close(db);
					return (EPKG_FATAL);
				}

				/* check if default repository exists */
				if (!is_attached(db->sqlite, "default")) {
					pkg_emit_error("no default repository defined");
					pkgdb_close(db);
					return (EPKG_FATAL);
				}
			}
		} else {
			/*
			 * Working on a single remote repository
			 */

			snprintf(remotepath, sizeof(remotepath), "%s/repo.sqlite", dbdir);

			if (access(remotepath, R_OK) != 0) {
				pkg_emit_errno("access", remotepath);
				pkgdb_close(db);
				return (EPKG_FATAL);
			}
			
			if (sql_exec(db->sqlite, "ATTACH '%q' AS 'remote';", remotepath) != EPKG_OK) {
				pkgdb_close(db);
				return (EPKG_FATAL);
			}
		}
	}

	*db_p = db;
	return (EPKG_OK);
}

void
pkgdb_close(struct pkgdb *db)
{
	if (db == NULL)
		return;

	if (db->sqlite != NULL) {
		if (db->type == PKGDB_REMOTE) {
			pkgdb_detach_remotes(db->sqlite);
		}

		sqlite3_close(db->sqlite);
	}

	sqlite3_shutdown();
	free(db);
}

static struct pkgdb_it *
pkgdb_it_new(struct pkgdb *db, sqlite3_stmt *s, int type)
{
	struct pkgdb_it *it;

	assert(db != NULL && s != NULL);

	if ((it = malloc(sizeof(struct pkgdb_it))) == NULL) {
		pkg_emit_errno("malloc", "pkgdb_it");
		sqlite3_finalize(s);
		return (NULL);
	}

	it->db = db;
	it->stmt = s;
	it->type = type;
	return (it);
}


int
pkgdb_it_next(struct pkgdb_it *it, struct pkg **pkg_p, int flags)
{
	struct pkg *pkg;
	int ret;

	assert(it != NULL);

	switch (sqlite3_step(it->stmt)) {
	case SQLITE_ROW:
		if (*pkg_p == NULL)
			pkg_new(pkg_p, it->type);
		else
			pkg_reset(*pkg_p, it->type);
		pkg = *pkg_p;

		populate_pkg(it->stmt, pkg);

		if (flags & PKG_LOAD_DEPS)
			if ((ret = pkgdb_load_deps(it->db, pkg)) != EPKG_OK)
				return (ret);

		if (flags & PKG_LOAD_RDEPS)
			if ((ret = pkgdb_load_rdeps(it->db, pkg)) != EPKG_OK)
				return (ret);

		if (flags & PKG_LOAD_FILES)
			if ((ret = pkgdb_load_files(it->db, pkg)) != EPKG_OK)
				return (ret);

		if (flags & PKG_LOAD_DIRS)
			if ((ret = pkgdb_load_dirs(it->db, pkg)) != EPKG_OK)
				return (ret);

		if (flags & PKG_LOAD_SCRIPTS)
			if ((ret = pkgdb_load_scripts(it->db, pkg)) != EPKG_OK)
				return (ret);

		if (flags & PKG_LOAD_OPTIONS)
			if ((ret = pkgdb_load_options(it->db, pkg)) != EPKG_OK)
				return (ret);

		if (flags & PKG_LOAD_MTREE)
			if ((ret = pkgdb_load_mtree(it->db, pkg)) != EPKG_OK)
				return (ret);

		if (flags & PKG_LOAD_CATEGORIES)
			if ((ret = pkgdb_load_category(it->db, pkg)) != EPKG_OK)
				return (ret);

		if (flags & PKG_LOAD_LICENSES)
			if ((ret = pkgdb_load_license(it->db, pkg)) != EPKG_OK)
				return (ret);

		if (flags & PKG_LOAD_USERS)
			if ((ret = pkgdb_load_user(it->db, pkg)) != EPKG_OK)
				return (ret);

		if (flags & PKG_LOAD_GROUPS)
			if ((ret = pkgdb_load_group(it->db, pkg)) != EPKG_OK)
				return (ret);

		return (EPKG_OK);
	case SQLITE_DONE:
		return (EPKG_END);
	default:
		ERROR_SQLITE(it->db->sqlite);
		return (EPKG_FATAL);
	}
}

void
pkgdb_it_free(struct pkgdb_it *it)
{
	if (it == NULL)
		return;

	if (it->db->writable == 1) {
		sql_exec(it->db->sqlite, "DROP TABLE IF EXISTS autoremove; "
			"DROP TABLE IF EXISTS delete_job; "
			"DROP TABLE IF EXISTS pkgjobs");
	}

	sqlite3_finalize(it->stmt);
	free(it);
}

struct pkgdb_it *
pkgdb_query(struct pkgdb *db, const char *pattern, match_t match)
{
	char sql[BUFSIZ];
	sqlite3_stmt *stmt;
	const char *comp = NULL;
	char *checkorigin = NULL;

	assert(db != NULL);
	assert(match == MATCH_ALL || pattern != NULL);

	if (pattern != NULL)
		checkorigin = strchr(pattern, '/');

	switch (match) {
	case MATCH_ALL:
		comp = "";
		break;
	case MATCH_EXACT:
		if (checkorigin == NULL)
			comp = " WHERE name = ?1 "
				"OR name || \"-\" || version = ?1";
		else
			comp = " WHERE origin = ?1";
		break;
	case MATCH_GLOB:
		if (checkorigin == NULL)
			comp = " WHERE name GLOB ?1 "
				"OR name || \"-\" || version GLOB ?1";
		else
			comp = " WHERE origin GLOB ?1";
		break;
	case MATCH_REGEX:
		if (checkorigin == NULL)
			comp = " WHERE name REGEXP ?1 "
				"OR name || \"-\" || version REGEXP ?1";
		else
			comp = " WHERE origin REGEXP ?1";
		break;
	case MATCH_EREGEX:
		if (checkorigin == NULL)
			comp = " WHERE EREGEXP(?1, name) "
				"OR EREGEXP(?1, name || \"-\" || version)";
		else
			comp = " WHERE EREGEXP(?1, origin)";
		break;
	}

	snprintf(sql, sizeof(sql),
			"SELECT id, origin, name, version, comment, desc, "
				"message, arch, osversion, maintainer, www, "
				"prefix, flatsize, licenselogic, automatic "
			"FROM packages AS p%s "
			"ORDER BY p.name;", comp);

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (NULL);
	}

	if (match != MATCH_ALL)
		sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);

	return (pkgdb_it_new(db, stmt, PKG_INSTALLED));
}

struct pkgdb_it *
pkgdb_query_which(struct pkgdb *db, const char *path)
{
	sqlite3_stmt *stmt;
	const char sql[] = ""
		"SELECT p.id, p.origin, p.name, p.version, p.comment, p.desc, "
			"p.message, p.arch, p.osversion, p.maintainer, p.www, "
			"p.prefix, p.flatsize "
			"FROM packages AS p, files AS f "
			"WHERE p.id = f.package_id "
				"AND f.path = ?1;";

	assert(db != NULL);

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (NULL);
	}

	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);

	return (pkgdb_it_new(db, stmt, PKG_INSTALLED));
}

int
pkgdb_is_dir_used(struct pkgdb *db, const char *dir, int64_t *res)
{
	sqlite3_stmt *stmt;
	int ret;

	const char sql[] = ""
		"SELECT count(package_id) FROM pkg_directories, directories "
		"WHERE directory_id = directories.id AND directories.path = ?1;";

	assert(db != NULL);

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	sqlite3_bind_text(stmt, 1, dir, -1, SQLITE_TRANSIENT);

	ret = sqlite3_step(stmt);

	if (ret == SQLITE_ROW)
		*res = sqlite3_column_int64(stmt, 0);

	sqlite3_finalize(stmt);

	if (ret != SQLITE_ROW) {
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}
	
	return (EPKG_OK);


}

int
pkgdb_load_deps(struct pkgdb *db, struct pkg *pkg)
{
	sqlite3_stmt *stmt = NULL;
	int ret = EPKG_OK;
	char sql[BUFSIZ];
	const char *reponame = NULL;
	const char *basesql = "" 
			"SELECT d.name, d.origin, d.version "
			"FROM '%s'.deps AS d "
			"WHERE d.package_id = ?1;";

	assert(db != NULL && pkg != NULL);

	pkg_get(pkg, PKG_REPONAME, &reponame);

	if (pkg->type == PKG_REMOTE)
		snprintf(sql, sizeof(sql), basesql, reponame);
	else
		snprintf(sql, sizeof(sql), basesql, "main");

	if (pkg->flags & PKG_LOAD_DEPS)
		return (EPKG_OK);

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	sqlite3_bind_int64(stmt, 1, pkg->rowid);

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		pkg_adddep(pkg, sqlite3_column_text(stmt, 0), sqlite3_column_text(stmt, 1),
				   sqlite3_column_text(stmt, 2));
	}
	sqlite3_finalize(stmt);

	if (ret != SQLITE_DONE) {
		pkg_list_free(pkg, PKG_DEPS);
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	pkg->flags |= PKG_LOAD_DEPS;
	return (EPKG_OK);
}

int
pkgdb_load_rdeps(struct pkgdb *db, struct pkg *pkg)
{
	sqlite3_stmt *stmt = NULL;
	int ret;
	const char *origin;
	const char sql[] = ""
		"SELECT p.name, p.origin, p.version "
		"FROM packages AS p, deps AS d "
		"WHERE p.id = d.package_id "
			"AND d.origin = ?1;";

	assert(db != NULL && pkg != NULL);
	assert(pkg->type == PKG_INSTALLED);

	if (pkg->flags & PKG_LOAD_RDEPS)
		return (EPKG_OK);

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	pkg_get(pkg, PKG_ORIGIN, &origin);
	sqlite3_bind_text(stmt, 1, origin, -1, SQLITE_STATIC);

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		pkg_addrdep(pkg, sqlite3_column_text(stmt, 0), sqlite3_column_text(stmt, 1),
				   sqlite3_column_text(stmt, 2));
	}
	sqlite3_finalize(stmt);

	if (ret != SQLITE_DONE) {
		pkg_list_free(pkg, PKG_RDEPS);
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	pkg->flags |= PKG_LOAD_RDEPS;
	return (EPKG_OK);
}

int
pkgdb_load_files(struct pkgdb *db, struct pkg *pkg)
{
	sqlite3_stmt *stmt = NULL;
	int ret;
	const char sql[] = ""
		"SELECT path, sha256 "
		"FROM files "
		"WHERE package_id = ?1 "
		"ORDER BY PATH ASC";

	assert(db != NULL && pkg != NULL);
	assert(pkg->type == PKG_INSTALLED);

	if (pkg->flags & PKG_LOAD_FILES)
		return (EPKG_OK);

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	sqlite3_bind_int64(stmt, 1, pkg->rowid);

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		pkg_addfile(pkg, sqlite3_column_text(stmt, 0), sqlite3_column_text(stmt, 1));
	}
	sqlite3_finalize(stmt);

	if (ret != SQLITE_DONE) {
		pkg_list_free(pkg, PKG_FILES);
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	pkg->flags |= PKG_LOAD_FILES;
	return (EPKG_OK);
}

int
pkgdb_load_dirs(struct pkgdb *db, struct pkg *pkg)
{
	const char sql[] = ""
		"SELECT path, try "
		"FROM pkg_directories, directories "
		"WHERE package_id = ?1 "
		"AND directory_id = directories.id "
		"ORDER by path DESC";
	sqlite3_stmt *stmt;
	int ret;

	assert(db != NULL && pkg != NULL);

	if (pkg->flags & PKG_LOAD_DIRS)
		return (EPKG_OK);

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	sqlite3_bind_int64(stmt, 1, pkg->rowid);

	while (( ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		pkg_adddir(pkg, sqlite3_column_text(stmt, 0), sqlite3_column_int(stmt, 1));
	}

	sqlite3_finalize(stmt);
	if (ret != SQLITE_DONE) {
		pkg_list_free(pkg, PKG_DIRS);
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	pkg->flags |= PKG_LOAD_DIRS;

	return (EPKG_OK);
}

int
pkgdb_load_license(struct pkgdb *db, struct pkg *pkg)
{
	char sql[BUFSIZ];
	const char *reponame = NULL;
	const char *basesql = "" 
			"SELECT name "
			"FROM '%s'.pkg_licenses, '%s'.licenses AS l "
			"WHERE package_id = ?1 "
			"AND license_id = l.id "
			"ORDER by name DESC";

	assert(db != NULL && pkg != NULL);

	if (pkg->type == PKG_REMOTE) {
		pkg_get(pkg, PKG_REPONAME, &reponame);
		snprintf(sql, sizeof(sql), basesql, reponame, reponame);
	} else
		snprintf(sql, sizeof(sql), basesql, "main", "main");

	return (load_val(db->sqlite, pkg, sql, PKG_LOAD_LICENSES, pkg_addlicense, PKG_LICENSES));
}

int
pkgdb_load_category(struct pkgdb *db, struct pkg *pkg)
{
	char sql[BUFSIZ];
	const char *reponame = NULL;
	const char *basesql = ""
			"SELECT name "
			"FROM '%s'.pkg_categories, '%s'.categories AS c "
			"WHERE package_id = ?1 "
			"AND category_id = c.id "
			"ORDER by name DESC";

	assert(db != NULL && pkg != NULL);

	if (pkg->type == PKG_REMOTE) {
		pkg_get(pkg, PKG_REPONAME, &reponame);
		snprintf(sql, sizeof(sql), basesql, reponame, reponame);
	} else
		snprintf(sql, sizeof(sql), basesql, "main", "main");

	return (load_val(db->sqlite, pkg, sql, PKG_LOAD_CATEGORIES, pkg_addcategory, PKG_CATEGORIES));
}

int
pkgdb_load_user(struct pkgdb *db, struct pkg *pkg)
{
	/*struct pkg_user *u = NULL;
	struct passwd *pwd = NULL;*/
	int ret;

	const char sql[] = ""
		"SELECT users.name "
		"FROM pkg_users, users "
		"WHERE package_id = ?1 "
		"AND user_id = users.id "
		"ORDER by name DESC";

	assert(db != NULL && pkg != NULL);

	ret = load_val(db->sqlite, pkg, sql, PKG_LOAD_USERS, pkg_adduser, PKG_USERS);

	/* TODO get user uidstr from local database */
/*	while (pkg_users(pkg, &u) == EPKG_OK) {
		pwd = getpwnam(pkg_user_name(u));
		if (pwd == NULL)
			continue;
		strlcpy(u->uidstr, pw_make(pwd), sizeof(u->uidstr));
	}*/

	return (ret);
}

int
pkgdb_load_group(struct pkgdb *db, struct pkg *pkg)
{
	struct pkg_group *g = NULL;
	struct group * grp = NULL;
	int ret;

	const char sql[] = ""
		"SELECT groups.name "
		"FROM pkg_groups, groups "
		"WHERE package_id = ?1 "
		"AND group_id = groups.id "
		"ORDER by name DESC";

	assert(db != NULL && pkg != NULL);

	ret = load_val(db->sqlite, pkg, sql, PKG_LOAD_GROUPS, pkg_addgroup, PKG_GROUPS);

	while (pkg_groups(pkg, &g) == EPKG_OK) {
		grp = getgrnam(pkg_group_name(g));
		if (grp == NULL)
			continue;
		strlcpy(g->gidstr, gr_make(grp), sizeof(g->gidstr));
	}

	return (ret);
}

int
pkgdb_load_scripts(struct pkgdb *db, struct pkg *pkg)
{
	sqlite3_stmt *stmt = NULL;
	int ret;
	const char sql[] = ""
		"SELECT script, type "
		"FROM scripts "
		"WHERE package_id = ?1";

	assert(db != NULL && pkg != NULL);
	assert(pkg->type == PKG_INSTALLED);

	if (pkg->flags & PKG_LOAD_SCRIPTS)
		return (EPKG_OK);

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	sqlite3_bind_int64(stmt, 1, pkg->rowid);

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		pkg_addscript(pkg, sqlite3_column_text(stmt, 0), sqlite3_column_int(stmt, 1));
	}
	sqlite3_finalize(stmt);

	if (ret != SQLITE_DONE) {
		pkg_list_free(pkg, PKG_SCRIPTS);
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	pkg->flags |= PKG_LOAD_SCRIPTS;
	return (EPKG_OK);
}

int
pkgdb_load_options(struct pkgdb *db, struct pkg *pkg)
{
	sqlite3_stmt *stmt = NULL;
	int ret = EPKG_OK;
	const char *reponame;
	char sql[BUFSIZ];
	const char *basesql = ""
		"SELECT option, value "
		"FROM '%s'.options "
		"WHERE package_id = ?1";

	assert(db != NULL && pkg != NULL);

	if (pkg->flags & PKG_LOAD_OPTIONS)
		return (EPKG_OK);

	if (pkg->type == PKG_REMOTE) {
		pkg_get(pkg, PKG_REPONAME, &reponame);
		snprintf(sql, sizeof(sql), basesql, reponame);
	} else {
		snprintf(sql, sizeof(sql), basesql, "main");
	}

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	sqlite3_bind_int64(stmt, 1, pkg->rowid);

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		pkg_addoption(pkg, sqlite3_column_text(stmt, 0),
					  sqlite3_column_text(stmt, 1));
	}
	sqlite3_finalize(stmt);

	if (ret != SQLITE_DONE) {
		pkg_list_free(pkg, PKG_OPTIONS);
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	pkg->flags |= PKG_LOAD_OPTIONS;
	return (EPKG_OK);
}

int
pkgdb_load_mtree(struct pkgdb *db, struct pkg *pkg)
{
	const char sql[] = ""
		"SELECT m.content "
		"FROM mtree AS m, packages AS p "
		"WHERE m.id = p.mtree_id "
			" AND p.id = ?1;";

	assert(db != NULL && pkg != NULL);
	assert(pkg->type == PKG_INSTALLED);

	return (load_val(db->sqlite, pkg, sql, PKG_LOAD_MTREE, pkg_set_mtree, -1));
}

int
pkgdb_register_pkg(struct pkgdb *db, struct pkg *pkg, int complete)
{
	struct pkg *pkg2 = NULL;
	struct pkg_dep *dep = NULL;
	struct pkg_file *file = NULL;
	struct pkg_dir *dir = NULL;
	struct pkg_script *script = NULL;
	struct pkg_option *option = NULL;
	struct pkg_category *category = NULL;
	struct pkg_license *license = NULL;
	struct pkg_user *user = NULL;
	struct pkg_group *group = NULL;
	struct pkgdb_it *it = NULL;

	sqlite3 *s;
	sqlite3_stmt *stmt_pkg = NULL;
	sqlite3_stmt *stmt_mtree = NULL;
	sqlite3_stmt *stmt_dep = NULL;
	sqlite3_stmt *stmt_file = NULL;
	sqlite3_stmt *stmt_script = NULL;
	sqlite3_stmt *stmt_option = NULL;
	sqlite3_stmt *stmt_dirs = NULL;
	sqlite3_stmt *stmt_dir = NULL;
	sqlite3_stmt *stmt_categories = NULL;
	sqlite3_stmt *stmt_cat = NULL;
	sqlite3_stmt *stmt_licenses = NULL;
	sqlite3_stmt *stmt_lic = NULL;
	sqlite3_stmt *stmt_user = NULL;
	sqlite3_stmt *stmt_users = NULL;
	sqlite3_stmt *stmt_groups = NULL;
	sqlite3_stmt *stmt_group = NULL;

	int ret;
	int retcode = EPKG_FATAL;
	int64_t package_id;

	const char sql_begin[] = "BEGIN;";
	const char sql_mtree[] = "INSERT OR IGNORE INTO mtree(content) VALUES(?1);";
	const char sql_dirs[] = "INSERT OR IGNORE INTO directories(path) VALUES(?1);";
	const char sql_pkg[] = ""
		"INSERT OR REPLACE INTO packages( "
			"origin, name, version, comment, desc, message, arch, "
			"osversion, maintainer, www, prefix, flatsize, automatic, licenselogic, "
			"mtree_id) "
		"VALUES( ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, "
		"(SELECT id from mtree where content = ?15));";
	const char sql_dep[] = ""
		"INSERT OR ROLLBACK INTO deps (origin, name, version, package_id) "
		"VALUES (?1, ?2, ?3, ?4);";
	const char sql_file[] = ""
		"INSERT OR ROLLBACK INTO files (path, sha256, package_id) "
		"VALUES (?1, ?2, ?3);";
	const char sql_script[] = ""
		"INSERT OR ROLLBACK INTO scripts (script, type, package_id) "
		"VALUES (?1, ?2, ?3);";
	const char sql_option[] = ""
		"INSERT OR ROLLBACK INTO options (option, value, package_id) "
		"VALUES (?1, ?2, ?3);";
	const char sql_dir[] = ""
		"INSERT OR ROLLBACK INTO pkg_directories(package_id, directory_id, try) "
		"VALUES (?1, "
		"(SELECT id FROM directories WHERE path = ?2), ?3);";
	const char sql_cat[] = "INSERT OR IGNORE INTO categories(name) VALUES(?1);";
	const char sql_category[] = ""
		"INSERT OR ROLLBACK INTO pkg_categories(package_id, category_id) "
		"VALUES (?1, (SELECT id FROM categories WHERE name = ?2));";
	const char sql_lic[] = "INSERT OR IGNORE INTO licenses(name) VALUES(?1);";
	const char sql_license[] = ""
		"INSERT OR ROLLBACK INTO pkg_licenses(package_id, license_id) "
		"VALUES (?1, (SELECT id FROM licenses WHERE name = ?2));";
	const char sql_user[] = "INSERT OR IGNORE INTO users(name) VALUES(?1);";
	const char sql_users[] = ""
		"INSERT OR ROLLBACK INTO pkg_users(package_id, user_id) "
		"VALUES (?1, (SELECT id FROM users WHERE name = ?2));";
	const char sql_group[] = "INSERT OR IGNORE INTO groups(name) VALUES(?1);";
	const char sql_groups[] = ""
		"INSERT OR ROLLBACK INTO pkg_groups(package_id, group_id) "
		"VALUES (?1, (SELECT id FROM groups WHERE name = ?2));";

	const char *mtree, *origin, *name, *version, *name2, *version2;
	const char *comment, *desc, *message;
	const char *arch, *osversion, *maintainer, *www, *prefix;

	bool automatic;
	int64_t flatsize;
	lic_t licenselogic;

	assert(db != NULL);

	s = db->sqlite;

	if (!complete && sql_exec(s, sql_begin) != EPKG_OK)
		return (EPKG_FATAL);

	/* insert mtree record if any */
	if (sqlite3_prepare_v2(s, sql_mtree, -1, &stmt_mtree, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	pkg_get(pkg, PKG_MTREE, &mtree, PKG_ORIGIN, &origin, PKG_VERSION, &version,
	    PKG_COMMENT, &comment, PKG_DESC, &desc, PKG_MESSAGE, &message,
	    PKG_ARCH, &arch, PKG_OSVERSION, &osversion, PKG_MAINTAINER, &maintainer,
	    PKG_WWW, &www, PKG_PREFIX, &prefix, PKG_FLATSIZE, &flatsize,
	    PKG_AUTOMATIC, &automatic, PKG_LICENSE_LOGIC, &licenselogic,
	    PKG_NAME, &name);

	sqlite3_bind_text(stmt_mtree, 1, mtree, -1, SQLITE_STATIC);

	if ((ret = sqlite3_step(stmt_mtree)) != SQLITE_DONE) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	/* Insert package record */
	if (sqlite3_prepare_v2(s, sql_pkg, -1, &stmt_pkg, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}
	sqlite3_bind_text(stmt_pkg, 1, origin, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt_pkg, 2, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt_pkg, 3, version, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt_pkg, 4, comment, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt_pkg, 5, desc, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt_pkg, 6, message, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt_pkg, 7, arch, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt_pkg, 8, osversion, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt_pkg, 9, maintainer, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt_pkg, 10, www,  -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt_pkg, 11, prefix, -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt_pkg, 12, flatsize);
	sqlite3_bind_int(stmt_pkg, 13, automatic);
	sqlite3_bind_int64(stmt_pkg, 14, licenselogic);
	sqlite3_bind_text(stmt_pkg, 15, mtree, -1, SQLITE_STATIC);

	if ((ret = sqlite3_step(stmt_pkg)) != SQLITE_DONE) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	package_id = sqlite3_last_insert_rowid(s);

	/*
	 * Insert dependencies list
	 */

	if (sqlite3_prepare_v2(s, sql_dep, -1, &stmt_dep, NULL) != SQLITE_OK) {

		ERROR_SQLITE(s);
		goto cleanup;
	}

	while (pkg_deps(pkg, &dep) == EPKG_OK) {
		sqlite3_bind_text(stmt_dep, 1, pkg_dep_get(dep, PKG_DEP_ORIGIN), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt_dep, 2, pkg_dep_get(dep, PKG_DEP_NAME), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt_dep, 3, pkg_dep_get(dep, PKG_DEP_VERSION), -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt_dep, 4, package_id);

		if ((ret = sqlite3_step(stmt_dep)) != SQLITE_DONE) {
			ERROR_SQLITE(s);
			goto cleanup;
		}
		sqlite3_reset(stmt_dep);
	}

	/*
	 * Insert files.
	 */

	if (sqlite3_prepare_v2(s, sql_file, -1, &stmt_file, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	while (pkg_files(pkg, &file) == EPKG_OK) {
		sqlite3_bind_text(stmt_file, 1, pkg_file_get(file, PKG_FILE_PATH), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt_file, 2, pkg_file_get(file, PKG_FILE_SUM), -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt_file, 3, package_id);

		if ((ret = sqlite3_step(stmt_file)) != SQLITE_DONE) {
			if (ret == SQLITE_CONSTRAINT) {
				if ((it = pkgdb_query_which(db, pkg_file_get(file, PKG_FILE_PATH))) == NULL) {
					ERROR_SQLITE(s);
				}
				if (( ret = pkgdb_it_next(it, &pkg2, PKG_LOAD_BASIC)) == EPKG_OK) {
					pkg_get(pkg2, PKG_NAME, &name2, PKG_VERSION, &version2);
					pkg_emit_error("%s-%s conflicts with %s-%s"
					    " (installs files into the same place). "
					    " Problematic file: %s",
					    name, version, name2, version2,
					    pkg_file_get(file, PKG_FILE_PATH));
					pkg_free(pkg2);
				} else {
					ERROR_SQLITE(s);
				}
				pkgdb_it_free(it);
			} else {
				ERROR_SQLITE(s);
			}
			goto cleanup;
		}
		sqlite3_reset(stmt_file);
	}

	/*
	 * Insert dirs.
	 */

	if (sqlite3_prepare_v2(s, sql_dirs, -1, &stmt_dirs, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	if (sqlite3_prepare_v2(s, sql_dir, -1, &stmt_dir, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	while (pkg_dirs(pkg, &dir) == EPKG_OK) {
		sqlite3_bind_text(stmt_dirs, 1, pkg_dir_path(dir), -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt_dir, 1, package_id);
		sqlite3_bind_text(stmt_dir, 2, pkg_dir_path(dir), -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt_dir, 3, pkg_dir_try(dir));
			
		if ((ret = sqlite3_step(stmt_dirs)) != SQLITE_DONE) {
			ERROR_SQLITE(s);
			goto cleanup;
		}
		if ((ret = sqlite3_step(stmt_dir)) != SQLITE_DONE) {
			if ( ret == SQLITE_CONSTRAINT) {
				pkg_emit_error("sqlite: constraint violation on dirs.path: %s",
			 					pkg_dir_path(dir));
			} else
				ERROR_SQLITE(s);
			goto cleanup;
		}
		sqlite3_reset(stmt_dir);
		sqlite3_reset(stmt_dirs);
	}

	/*
	 * Insert categories
	 */

	if (sqlite3_prepare_v2(s, sql_category, -1, &stmt_cat, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}
	if (sqlite3_prepare_v2(s, sql_cat, -1, &stmt_categories, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	while (pkg_categories(pkg, &category) == EPKG_OK) {
		sqlite3_bind_text(stmt_categories, 1, pkg_category_name(category), -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt_cat, 1, package_id);
		sqlite3_bind_text(stmt_cat, 2, pkg_category_name(category), -1, SQLITE_STATIC);

		if ((ret = sqlite3_step(stmt_categories)) != SQLITE_DONE) {
			if (ret == SQLITE_CONSTRAINT) {
				pkg_emit_error("sqlite: constraint violation on categories.name: %s",
						pkg_category_name(category));
			} else
				ERROR_SQLITE(s);
			goto cleanup;
		}
		if (( ret = sqlite3_step(stmt_cat)) != SQLITE_DONE) {
			ERROR_SQLITE(s);
			goto cleanup;
		}
		sqlite3_reset(stmt_cat);
		sqlite3_reset(stmt_categories);
	}

	/*
	 * Insert licenses
	 */
	if (sqlite3_prepare_v2(s, sql_lic, -1, &stmt_licenses, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}
	if (sqlite3_prepare_v2(s, sql_license, -1, &stmt_lic, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	while (pkg_licenses(pkg, &license) == EPKG_OK) {
		sqlite3_bind_text(stmt_licenses, 1, pkg_license_name(license), -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt_lic, 1, package_id);
		sqlite3_bind_text(stmt_lic, 2, pkg_license_name(license), -1, SQLITE_STATIC);

		if ((ret = sqlite3_step(stmt_licenses)) != SQLITE_DONE) {
			if (ret == SQLITE_CONSTRAINT) {
				pkg_emit_error("sqlite: constraint violation on licenses.name: %s",
						pkg_license_name(license));
			} else
				ERROR_SQLITE(s);
			goto cleanup;
		}
		if (( ret = sqlite3_step(stmt_lic)) != SQLITE_DONE) {
			ERROR_SQLITE(s);
			goto cleanup;
		}
		sqlite3_reset(stmt_lic);
		sqlite3_reset(stmt_licenses);
	}

	/*
	 * Insert users
	 */
	if (sqlite3_prepare_v2(s, sql_user, -1, &stmt_user, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}
	if (sqlite3_prepare_v2(s, sql_users, -1, &stmt_users, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	while (pkg_users(pkg, &user) == EPKG_OK) {
		sqlite3_bind_text(stmt_user, 1, pkg_user_name(user), -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt_users, 1, package_id);
		sqlite3_bind_text(stmt_users, 2, pkg_user_name(user), -1, SQLITE_STATIC);

		if ((ret = sqlite3_step(stmt_user)) != SQLITE_DONE) {
			if (ret == SQLITE_CONSTRAINT) {
				pkg_emit_error("sqlite: constraint violation on users.name: %s",
						pkg_user_name(user));
			} else
				ERROR_SQLITE(s);
			goto cleanup;
		}
		if (( ret = sqlite3_step(stmt_users)) != SQLITE_DONE) {
			ERROR_SQLITE(s);
			goto cleanup;
		}
		sqlite3_reset(stmt_user);
		sqlite3_reset(stmt_users);
	}

	/*
	 * Insert groups
	 */
	if (sqlite3_prepare_v2(s, sql_group, -1, &stmt_group, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}
	if (sqlite3_prepare_v2(s, sql_groups, -1, &stmt_groups, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	while (pkg_groups(pkg, &group) == EPKG_OK) {
		sqlite3_bind_text(stmt_group, 1, pkg_group_name(group), -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt_groups, 1, package_id);
		sqlite3_bind_text(stmt_groups, 2, pkg_group_name(group), -1, SQLITE_STATIC);

		if ((ret = sqlite3_step(stmt_group)) != SQLITE_DONE) {
			if (ret == SQLITE_CONSTRAINT) {
				pkg_emit_error("sqlite: constraint violation on groups.name: %s",
						pkg_group_name(group));
			} else
				ERROR_SQLITE(s);
			goto cleanup;
		}
		if (( ret = sqlite3_step(stmt_groups)) != SQLITE_DONE) {
			ERROR_SQLITE(s);
			goto cleanup;
		}
		sqlite3_reset(stmt_group);
		sqlite3_reset(stmt_groups);
	}

	/*
	 * Insert scripts
	 */

	if (sqlite3_prepare_v2(s, sql_script, -1, &stmt_script, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	while (pkg_scripts(pkg, &script) == EPKG_OK) {
		sqlite3_bind_text(stmt_script, 1, pkg_script_data(script), -1, SQLITE_STATIC);
		sqlite3_bind_int(stmt_script, 2, pkg_script_type(script));
		sqlite3_bind_int64(stmt_script, 3, package_id);

		if (sqlite3_step(stmt_script) != SQLITE_DONE) {
			ERROR_SQLITE(s);
			goto cleanup;
		}
		sqlite3_reset(stmt_script);
	}

	/*
	 * Insert options
	 */

	if (sqlite3_prepare_v2(s, sql_option, -1, &stmt_option, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		goto cleanup;
	}

	while (pkg_options(pkg, &option) == EPKG_OK) {
		sqlite3_bind_text(stmt_option, 1, pkg_option_opt(option), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt_option, 2, pkg_option_value(option), -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt_option, 3, package_id);

		if (sqlite3_step(stmt_option) != SQLITE_DONE) {
			ERROR_SQLITE(s);
			goto cleanup;
		}
		sqlite3_reset(stmt_option);
	}

	retcode = EPKG_OK;

	cleanup:

	if (stmt_mtree != NULL)
		sqlite3_finalize(stmt_mtree);

	if (stmt_pkg != NULL)
		sqlite3_finalize(stmt_pkg);

	if (stmt_dep != NULL)
		sqlite3_finalize(stmt_dep);

	if (stmt_file != NULL)
		sqlite3_finalize(stmt_file);

	if (stmt_script != NULL)
		sqlite3_finalize(stmt_script);

	if (stmt_option != NULL)
		sqlite3_finalize(stmt_option);
	
	if (stmt_dirs != NULL)
		sqlite3_finalize(stmt_dirs);

	if (stmt_dir != NULL)
		sqlite3_finalize(stmt_dir);

	if (stmt_cat != NULL)
		sqlite3_finalize(stmt_cat);

	if (stmt_categories != NULL)
		sqlite3_finalize(stmt_categories);

	if (stmt_lic != NULL)
		sqlite3_finalize(stmt_lic);

	if (stmt_licenses != NULL)
		sqlite3_finalize(stmt_licenses);

	if (stmt_groups != NULL)
		sqlite3_finalize(stmt_groups);

	if (stmt_users != NULL)
		sqlite3_finalize(stmt_users);

	return (retcode);
}

int
pkgdb_register_finale(struct pkgdb *db, int retcode)
{
	int ret = EPKG_OK;
	const char *cmd;

	assert(db != NULL);

	cmd = (retcode == EPKG_OK) ? "COMMIT;" : "ROLLBACK;";
	ret = sql_exec(db->sqlite, cmd);

	return (ret);
}

int
pkgdb_unregister_pkg(struct pkgdb *db, const char *origin)
{
	sqlite3_stmt *stmt_del;
	int ret;
	const char sql[] = "DELETE FROM packages WHERE origin = ?1;";

	assert(db != NULL);
	assert(origin != NULL);

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt_del, NULL) != SQLITE_OK){
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	sqlite3_bind_text(stmt_del, 1, origin, -1, SQLITE_STATIC);

	ret = sqlite3_step(stmt_del);
	sqlite3_finalize(stmt_del);

	if (ret != SQLITE_DONE) {
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	/* cleanup directories */
	if (sql_exec(db->sqlite, "DELETE from directories WHERE id NOT IN (SELECT DISTINCT directory_id FROM pkg_directories);") != EPKG_OK)
		return (EPKG_FATAL);

	if (sql_exec(db->sqlite, "DELETE from categories WHERE id NOT IN (SELECT DISTINCT category_id FROM pkg_categories);") != EPKG_OK)
		return (EPKG_FATAL);

	if (sql_exec(db->sqlite, "DELETE from licenses WHERE id NOT IN (SELECT DISTINCT license_id FROM pkg_licenses);") != EPKG_OK)
		return (EPKG_FATAL);

	if (sql_exec(db->sqlite, "DELETE FROM mtree WHERE id NOT IN (SELECT DISTINCT mtree_id FROM packages);") != EPKG_OK)
		return (EPKG_FATAL);

	/* TODO print the users that are not used anymore */
	if (sql_exec(db->sqlite, "DELETE FROM users WHERE id NOT IN (SELECT DISTINCT user_id FROM pkg_users);") != EPKG_OK)
		return (EPKG_FATAL);

	/* TODO print the groups trhat are not used anymore */
	if (sql_exec(db->sqlite, "DELETE FROM groups WHERE id NOT IN (SELECT DISTINCT group_id FROM pkg_groups);") != EPKG_OK)
		return (EPKG_FATAL);

	return (EPKG_OK);
}

int
sql_exec(sqlite3 *s, const char *sql, ...)
{
	va_list ap;
	const char *sql_to_exec;
	char *sqlbuf = NULL;
	char *errmsg;
	int ret = EPKG_FATAL;

	assert(s != NULL);
	assert(sql != NULL);

	if (strchr(sql, '%') != NULL) {
		va_start(ap, sql);
		sqlbuf = sqlite3_vmprintf(sql, ap);
		va_end(ap);
		sql_to_exec = sqlbuf;
	} else {
		sql_to_exec = sql;
	}

	if (sqlite3_exec(s, sql_to_exec, NULL, NULL, &errmsg) != SQLITE_OK) {
		pkg_emit_error("sqlite: %s", errmsg);
		sqlite3_free(errmsg);
		goto cleanup;
	}

	ret = EPKG_OK;

	cleanup:
	if (sqlbuf != NULL)
		free(sqlbuf);

	return (ret);
}

static bool
is_attached(sqlite3 *s, const char *name)
{
	sqlite3_stmt *stmt;
	const char *dbname;

	assert(s != NULL);

	if (sqlite3_prepare_v2(s, "PRAGMA database_list;", -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		return false;
	}

	while (sqlite3_step(stmt) != SQLITE_DONE) {
		dbname = sqlite3_column_text(stmt, 1);
		if (!strcmp(dbname, name)) {
			sqlite3_finalize(stmt);
			return (true);
		}
	}

	sqlite3_finalize(stmt);

	return (false);
}

static int
sql_on_all_attached_db(sqlite3 *s, struct sbuf *sql, const char *multireposql) {
	sqlite3_stmt *stmt;
	const char *dbname;
	bool first = true;

	assert(s != NULL);

	if (sqlite3_prepare_v2(s, "PRAGMA database_list;", -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		return (EPKG_FATAL);
	}

	while (sqlite3_step(stmt) != SQLITE_DONE) {
		dbname = sqlite3_column_text(stmt, 1);
		if ((strcmp(dbname, "main") == 0) || (strcmp(dbname, "temp") == 0))
			continue;

		if (!first) {
			sbuf_cat(sql, " UNION ALL ");
		} else {
			first = false;
		}
		sbuf_printf(sql, multireposql, dbname, dbname);
	}

	sqlite3_finalize(stmt);

	return (EPKG_OK);
}

static void
pkgdb_detach_remotes(sqlite3 *s)
{
	sqlite3_stmt *stmt;
	struct sbuf *sql = NULL;
	const char *dbname;

	assert(s != NULL);

	if (sqlite3_prepare_v2(s, "PRAGMA database_list;", -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		return;
	}

	sql = sbuf_new_auto();

	while (sqlite3_step(stmt) != SQLITE_DONE) {
		dbname = sqlite3_column_text(stmt, 1);
		if ((strcmp(dbname, "main") == 0) ||
		    (strcmp(dbname, "temp") == 0))
			continue;

		sbuf_clear(sql);
		sbuf_printf(sql, "DETACH '%s';", dbname);
		sbuf_finish(sql);
		sql_exec(s, sbuf_get(sql));
	}

	sqlite3_finalize(stmt);

	sbuf_delete(sql);
}

static int
get_pragma(sqlite3 *s, const char *sql, int64_t *res)
{
	sqlite3_stmt *stmt;
	int ret;

	assert(s != NULL && sql != NULL);

	if (sqlite3_prepare_v2(s, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(s);
		return (EPKG_OK);
	}

	ret = sqlite3_step(stmt);

	if (ret == SQLITE_ROW)
		*res = sqlite3_column_int64(stmt, 0);

	sqlite3_finalize(stmt);

	if (ret != SQLITE_ROW) {
		ERROR_SQLITE(s);
		return (EPKG_FATAL);
	}

	return (EPKG_OK);
}

int
pkgdb_compact(struct pkgdb *db)
{
	int64_t page_count = 0;
	int64_t freelist_count = 0;

	assert(db != NULL);

	if (get_pragma(db->sqlite, "PRAGMA page_count;", &page_count) != EPKG_OK)
		return (EPKG_FATAL);

	if (get_pragma(db->sqlite, "PRAGMA freelist_count;", &freelist_count) !=
		EPKG_OK)
		return (EPKG_FATAL);

	/*
	 * Only compact if we will save 25% (or more) of the current used space.
	 */
	if (freelist_count / (float)page_count < 0.25)
		return (EPKG_OK);

	return (sql_exec(db->sqlite, "VACUUM;"));
}

static int
create_temporary_pkgjobs(sqlite3 *s)
{
	int ret;

	assert(s != NULL);

	ret = sql_exec(s, "DROP TABLE IF EXISTS pkgjobs;"
			"CREATE TEMPORARY TABLE IF NOT EXISTS pkgjobs (pkgid INTEGER, "
			"origin TEXT UNIQUE NOT NULL, name TEXT, version TEXT, "
			"comment TEXT, desc TEXT, message TEXT, "
			"arch TEXT, osversion TEXT, maintainer TEXT, "
			"www TEXT, prefix TEXT, flatsize INTEGER, newversion TEXT, "
			"newflatsize INTEGER, pkgsize INTEGER, cksum TEXT, repopath TEXT, automatic INTEGER, "
			"dbname TEXT);");

	return (ret);
}

struct pkgdb_it *
pkgdb_query_installs(struct pkgdb *db, match_t match, int nbpkgs, char **pkgs, const char *repo)
{
	sqlite3_stmt *stmt = NULL;
	int i = 0;
	struct sbuf *sql = sbuf_new_auto();
	const char *how = NULL;
	const char *reponame = NULL;
	bool multirepos_enabled = false;

	const char finalsql[] = "SELECT pkgid AS id, origin, name, version, "
		"comment, desc, message, arch, osversion, maintainer, "
		"www, prefix, flatsize, newversion, newflatsize, pkgsize, "
		"cksum, repopath, automatic, (select count(*) FROM '%s'.deps as d WHERE d.origin = pkgjobs.origin) as weight, "
		"'%s' AS dbname FROM pkgjobs ORDER BY weight DESC;";
       
	const char main_sql[] = "INSERT OR IGNORE INTO pkgjobs (pkgid, origin, name, version, comment, desc, arch, "
			"osversion, maintainer, www, prefix, flatsize, pkgsize, "
			"cksum, repopath, automatic) "
			"SELECT id, origin, name, version, comment, desc, "
			"arch, osversion, maintainer, www, prefix, flatsize, pkgsize, "
			"cksum, path, 0 FROM '%s'.packages WHERE ";

	const char deps_sql[] = "INSERT OR IGNORE INTO pkgjobs (pkgid, origin, name, version, comment, desc, arch, "
				"osversion, maintainer, www, prefix, flatsize, pkgsize, "
				"cksum, repopath, automatic) "
				"SELECT DISTINCT r.id, r.origin, r.name, r.version, r.comment, r.desc, "
				"r.arch, r.osversion, r.maintainer, r.www, r.prefix, r.flatsize, r.pkgsize, "
				"r.cksum, r.path, 1 "
				"FROM '%s'.packages AS r where r.origin IN "
				"(SELECT d.origin FROM '%s'.deps AS d, pkgjobs AS j WHERE d.package_id = j.pkgid) "
				"AND (SELECT origin FROM main.packages WHERE origin=r.origin AND version=r.version) IS NULL;";

	assert(db != NULL);

	if (db->type != PKGDB_REMOTE) {
		pkg_emit_error("remote database not attached (misuse)");
		return (NULL);
	}

	/* Working on multiple repositories */
	pkg_config_bool(PKG_CONFIG_MULTIREPOS, &multirepos_enabled);

	if (multirepos_enabled) {
		if (repo != NULL) {
			if (!is_attached(db->sqlite, repo)) {
				pkg_emit_error("repository '%s' does not exists", repo);
				return (NULL);
			}

			reponame = repo;
		} else {
			/* default repository in multi-repos is 'default' */
			reponame = "default";
		}
	} else {
		/* default repository in single-repo is 'remote' */
		reponame = "remote";
	}

	sbuf_printf(sql, main_sql, reponame);

	switch (match) {
		case MATCH_ALL:
			how = NULL;
			break;
		case MATCH_EXACT:
			how = "%s = ?1";
			break;
		case MATCH_GLOB:
			how = "%s GLOB ?1";
			break;
		case MATCH_REGEX:
			how = "%s REGEXP ?1";
			break;
		case MATCH_EREGEX:
			how = "EREGEXP(?1, %s)";
			break;
	}

	create_temporary_pkgjobs(db->sqlite);

	sbuf_printf(sql, how, "name");
	sbuf_cat(sql, " OR ");
	sbuf_printf(sql, how, "origin");
	sbuf_cat(sql, " OR ");
	sbuf_printf(sql, how, "name || \"-\" || version");
	sbuf_finish(sql);

	for (i = 0; i < nbpkgs; i++) {
		if (sqlite3_prepare_v2(db->sqlite, sbuf_get(sql), -1, &stmt, NULL) != SQLITE_OK) {
			ERROR_SQLITE(db->sqlite);
			return (NULL);
		}
		sqlite3_bind_text(stmt, 1, pkgs[i], -1, SQLITE_STATIC);
		while (sqlite3_step(stmt) != SQLITE_DONE);
	}

	sqlite3_finalize(stmt);
	sbuf_clear(sql);

	/* Remove packages already installed and in the latest version */
	sql_exec(db->sqlite, "DELETE from pkgjobs where (select p.origin from main.packages as p where p.origin=pkgjobs.origin and version=pkgjobs.version) IS NOT NULL;");

	/* Append dependencies */
	sbuf_reset(sql);
	sbuf_printf(sql, deps_sql, reponame, reponame);

	do {
		sql_exec(db->sqlite, sbuf_get(sql));
	} while (sqlite3_changes(db->sqlite) != 0);


	/* Determine if there is an upgrade needed */
	sql_exec(db->sqlite, "INSERT OR REPLACE INTO pkgjobs (pkgid, origin, name, version, comment, desc, message, arch, "
			"osversion, maintainer, www, prefix, flatsize, newversion, newflatsize, pkgsize, "
			"cksum, repopath, automatic) "
			"SELECT l.id, l.origin, l.name, l.version, l.comment, l.desc, l.message, l.arch, "
			"l.osversion, l.maintainer, l.www, l.prefix, l.flatsize, r.version AS newversion, "
			"r.flatsize AS newflatsize, r.pkgsize, r.cksum, r.repopath, r.automatic "
			"FROM main.packages AS l, pkgjobs AS r WHERE l.origin = r.origin "
			"AND (PKGLT(l.version, r.version) OR (l.name != r.name))");

	sbuf_reset(sql);
	sbuf_printf(sql, finalsql, reponame, reponame);

	if (sqlite3_prepare_v2(db->sqlite, sbuf_get(sql), -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (NULL);
	}

	sbuf_finish(sql);
	sbuf_delete(sql);

	return (pkgdb_it_new(db, stmt, PKG_REMOTE));
}

struct pkgdb_it *
pkgdb_query_upgrades(struct pkgdb *db, const char *repo)
{
	sqlite3_stmt *stmt = NULL;
	struct sbuf *sql = sbuf_new_auto();
	const char *reponame = NULL;
	bool multirepos_enabled = false;

	assert(db != NULL);

	if (db->type != PKGDB_REMOTE) {
		pkg_emit_error("remote database not attached (misuse)");
		return (NULL);
	}

	const char finalsql[] = "select pkgid as id, origin, name, version, "
		"comment, desc, message, arch, osversion, maintainer, "
		"www, prefix, flatsize, newversion, newflatsize, pkgsize, "
		"cksum, repopath, automatic, (select count(*) from '%s'.deps as d where d.origin = pkgjobs.origin) as weight, "
		"'%s' AS dbname FROM pkgjobs order by weight DESC;";
		
	const char pkgjobs_sql_1[] = "INSERT OR IGNORE INTO pkgjobs (pkgid, origin, name, version, comment, desc, arch, "
			"osversion, maintainer, www, prefix, flatsize, pkgsize, "
			"cksum, repopath, automatic) "
			"SELECT id, origin, name, version, comment, desc, "
			"arch, osversion, maintainer, www, prefix, flatsize, pkgsize, "
			"cksum, path, 0 FROM '%s'.packages WHERE origin IN (select origin from main.packages)";

	const char pkgjobs_sql_2[] = "INSERT OR IGNORE INTO pkgjobs (pkgid, origin, name, version, comment, desc, arch, "
				"osversion, maintainer, www, prefix, flatsize, pkgsize, "
				"cksum, repopath, automatic) "
				"SELECT DISTINCT r.id, r.origin, r.name, r.version, r.comment, r.desc, "
				"r.arch, r.osversion, r.maintainer, r.www, r.prefix, r.flatsize, r.pkgsize, "
				"r.cksum, r.path, 1 "
				"FROM '%s'.packages AS r where r.origin IN "
				"(SELECT d.origin from '%s'.deps AS d, pkgjobs as j WHERE d.package_id = j.pkgid) "
				"AND (SELECT p.origin from main.packages as p WHERE p.origin=r.origin AND version=r.version) IS NULL;";

	const char pkgjobs_sql_3[] = "INSERT OR REPLACE INTO pkgjobs (pkgid, origin, name, version, comment, desc, message, arch, "
			"osversion, maintainer, www, prefix, flatsize, newversion, newflatsize, pkgsize, "
			"cksum, repopath, automatic) "
			"SELECT l.id, l.origin, l.name, l.version, l.comment, l.desc, l.message, l.arch, "
			"l.osversion, l.maintainer, l.www, l.prefix, l.flatsize, r.version AS newversion, "
			"r.flatsize AS newflatsize, r.pkgsize, r.cksum, r.repopath, r.automatic "
			"FROM main.packages AS l, pkgjobs AS r WHERE l.origin = r.origin "
			"AND (PKGLT(l.version, r.version) OR (l.name != r.name))";

	/* Working on multiple repositories */
	pkg_config_bool(PKG_CONFIG_MULTIREPOS, &multirepos_enabled);

	if (multirepos_enabled) {
		if (repo != NULL) {
			if (!is_attached(db->sqlite, repo)) {
				pkg_emit_error("repository '%s' does not exists", repo);
				return (NULL);
			}

			reponame = repo;
		} else {
			/* default repository in multi-repos is 'default' */
			reponame = "default";
		}
	} else {
		/* default repository in single-repo is 'remote' */
		reponame = "remote";
	}

	create_temporary_pkgjobs(db->sqlite);

	sbuf_printf(sql, pkgjobs_sql_1, reponame);
	sql_exec(db->sqlite, sbuf_get(sql));

	/* Remove packages already installed and in the latest version */
	sql_exec(db->sqlite, "DELETE from pkgjobs where (select p.origin from main.packages as p where p.origin=pkgjobs.origin and version=pkgjobs.version) IS NOT NULL;");

	sbuf_reset(sql);
	sbuf_printf(sql, pkgjobs_sql_2, reponame, reponame);

	do {
		sql_exec(db->sqlite, sbuf_get(sql));
	} while (sqlite3_changes(db->sqlite) != 0);

	/* Determine if there is an upgrade needed */
	sql_exec(db->sqlite, pkgjobs_sql_3);

	sbuf_reset(sql);
	sbuf_printf(sql, finalsql, reponame, reponame);

	if (sqlite3_prepare_v2(db->sqlite, sbuf_get(sql), -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (NULL);
	}

	sbuf_finish(sql);
	sbuf_delete(sql);

	return (pkgdb_it_new(db, stmt, PKG_REMOTE));
}

struct pkgdb_it *
pkgdb_query_downgrades(struct pkgdb *db, const char *repo)
{
	struct sbuf *sql = sbuf_new_auto();
	const char *reponame = NULL;
	sqlite3_stmt *stmt = NULL;
	bool multirepos_enabled = false;

	assert(db != NULL);

	if (db->type != PKGDB_REMOTE) {
		pkg_emit_error("remote database not attached (misuse)");
		return (NULL);
	}

	const char finalsql[] = ""
		"SELECT l.id, l.origin AS origin, l.name AS name, l.version AS version, l.comment AS comment, l.desc AS desc, "
		"l.message AS message, l.arch AS arch, l.osversion AS osversion, l.maintainer AS maintainer, "
		"l.www AS www, l.prefix AS prefix, l.flatsize AS flatsize, r.version AS version, r.flatsize AS newflatsize, "
		"r.pkgsize AS pkgsize, r.path AS repopath, '%s' AS dbname "
		"FROM main.packages AS l, "
		"'%s'.packages AS r "
		"WHERE l.origin = r.origin "
		"AND PKGGT(l.version, r.version)";

	/* Working on multiple repositories */
	pkg_config_bool(PKG_CONFIG_MULTIREPOS, &multirepos_enabled);

	if (multirepos_enabled) {
		if (repo != NULL) {
			if (!is_attached(db->sqlite, repo)) {
				pkg_emit_error("repository '%s' does not exists", repo);
				return (NULL);
			}

			reponame = repo;
		} else {
			/* default repository in multi-repos is 'default' */
			reponame = "default";
		}
	} else {
		/* default repository in single-repo is 'remote' */
		reponame = "remote";
	}

	sbuf_printf(sql, finalsql, reponame, reponame);
	sbuf_finish(sql);

	if (sqlite3_prepare_v2(db->sqlite, sbuf_get(sql), -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (NULL);
	}

	return (pkgdb_it_new(db, stmt, PKG_REMOTE));
}

struct pkgdb_it *
pkgdb_query_autoremove(struct pkgdb *db)
{
	sqlite3_stmt *stmt = NULL;
	int weight = 0;

	assert(db != NULL);

	const char sql[] = ""
		"SELECT id, p.origin, name, version, comment, desc, "
		"message, arch, osversion, maintainer, www, prefix, "
		"flatsize FROM packages as p, autoremove where id = pkgid ORDER BY weight ASC;";

	sql_exec(db->sqlite, "DROP TABLE IF EXISTS autoremove; "
			"CREATE TEMPORARY TABLE IF NOT EXISTS autoremove ("
			"origin TEXT UNIQUE NOT NULL, pkgid INTEGER, weight INTEGER);");

	do {
		sql_exec(db->sqlite, "INSERT OR IGNORE into autoremove(origin, pkgid, weight) "
				"SELECT distinct origin, id, %d FROM packages WHERE automatic=1 AND "
				"origin NOT IN (SELECT DISTINCT deps.origin FROM deps WHERE "
				" deps.origin = packages.origin);"
				, weight);
	} while (sqlite3_changes(db->sqlite) != 0);

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (NULL);
	}

	return (pkgdb_it_new(db, stmt, PKG_INSTALLED));
}

struct pkgdb_it *
pkgdb_query_delete(struct pkgdb *db, match_t match, int nbpkgs, char **pkgs, int recursive)
{
	sqlite3_stmt *stmt = NULL;

	struct sbuf *sql = sbuf_new_auto();
	const char *how = NULL;
	int i = 0;

	assert(db != NULL);

	const char sqlsel[] = ""
		"SELECT id, p.origin, name, version, comment, desc, "
		"message, arch, osversion, maintainer, www, prefix, "
		"flatsize, (select count(*) from deps AS d where d.origin=del.origin) as weight FROM packages as p, delete_job as del where id = pkgid "
		"ORDER BY weight ASC;";

	sbuf_cat(sql, "INSERT OR IGNORE INTO delete_job (origin, pkgid) "
			"SELECT p.origin, p.id FROM packages as p ");

	switch (match) {
		case MATCH_ALL:
			how = NULL;
			break;
		case MATCH_EXACT:
			how = "%s = ?1";
			break;
		case MATCH_GLOB:
			how = "%s GLOB ?1";
			break;
		case MATCH_REGEX:
			how = "%s REGEXP ?1";
			break;
		case MATCH_EREGEX:
			how = "EREGEXP(?1, %s)";
			break;
	}

	sql_exec(db->sqlite, "DROP TABLE IF EXISTS delete_job; "
			"CREATE TEMPORARY TABLE IF NOT EXISTS delete_job ("
			"origin TEXT UNIQUE NOT NULL, pkgid INTEGER);"
			);

	if (how != NULL) {
		sbuf_cat(sql, " WHERE ");
		sbuf_printf(sql, how, "p.name");
		sbuf_cat(sql, " OR ");
		sbuf_printf(sql, how, "p.origin");
		sbuf_cat(sql, " OR ");
		sbuf_printf(sql, how, "p.name || \"-\" || p.version");
		sbuf_finish(sql);

		for (i = 0; i < nbpkgs; i++) {
			if (sqlite3_prepare_v2(db->sqlite, sbuf_get(sql), -1, &stmt, NULL) != SQLITE_OK) {
				ERROR_SQLITE(db->sqlite);
				return (NULL);
			}
			sqlite3_bind_text(stmt, 1, pkgs[i], -1, SQLITE_STATIC);
			while (sqlite3_step(stmt) != SQLITE_DONE);
		}
	} else {
		sbuf_finish(sql);
		if (sqlite3_prepare_v2(db->sqlite, sbuf_get(sql), -1, &stmt, NULL) != SQLITE_OK) {
			ERROR_SQLITE(db->sqlite);
			return (NULL);
		}
		sqlite3_bind_text(stmt, 1, pkgs[i], -1, SQLITE_STATIC);
		while (sqlite3_step(stmt) != SQLITE_DONE);
	}

	sqlite3_finalize(stmt);

	if (recursive) {
		do {
			sql_exec(db->sqlite, "INSERT OR IGNORE INTO delete_job(origin, pkgid) "
					"SELECT p.origin, p.id FROM deps AS d, packages AS p, delete_job AS del WHERE "
					"d.origin=del.origin AND p.id = d.package_id");
		} while (sqlite3_changes(db->sqlite) != 0);
	}

	if (sqlite3_prepare_v2(db->sqlite, sqlsel, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (NULL);
	}

	sbuf_finish(sql);
	sbuf_delete(sql);

	return (pkgdb_it_new(db, stmt, PKG_INSTALLED));
}

static int
pkgdb_rquery_build_search_query(struct sbuf *sql, match_t match, unsigned int field)
{
	const char *how = NULL;
	const char *what = NULL;

	switch (match) {
		case MATCH_ALL:
			how = NULL;
			break;
		case MATCH_EXACT:
			how = "%s = ?1";
			break;
		case MATCH_GLOB:
			how = "%s GLOB ?1";
			break;
		case MATCH_REGEX:
			how = "%s REGEXP ?1";
			break;
		case MATCH_EREGEX:
			how = "EREGEXP(?1, %s)";
			break;
	}

	switch(field) {
		case FIELD_NONE:
			what = NULL;
			break;
		case FIELD_ORIGIN:
			what = "origin";
			break;
		case FIELD_NAME:
			what = "name";
			break;
		case FIELD_NAMEVER:
			what = "name || \"-\" || version";
			break;
		case FIELD_COMMENT:
			what = "comment";
			break;
		case FIELD_DESC:
			what = "desc";
			break;
	}

	if (what != NULL && how != NULL)
		sbuf_printf(sql, how, what);

	return (EPKG_OK);
}

struct pkgdb_it *
pkgdb_rquery(struct pkgdb *db, const char *pattern, match_t match, unsigned int field, const char *reponame)
{
	sqlite3_stmt *stmt = NULL;
	struct sbuf *sql = NULL;
	bool multirepos_enabled = false;
	const char *basesql = ""
				"SELECT id, origin, name, version, comment, "
					"prefix, desc, arch, osversion, maintainer, www, "
					"licenselogic, flatsize AS newflatsize, pkgsize, "
					"cksum, path AS repopath ";
	const char *multireposql = ""
				"SELECT id, origin, name, version, comment, "
					"prefix, desc, arch, osversion, maintainer, www, "
					"licenselogic, flatsize, pkgsize, "
					"cksum, path, '%s' AS dbname "
					"FROM '%s'.packages ";

	assert(pattern != NULL && pattern[0] != '\0');

	if (db->type != PKGDB_REMOTE) {
		pkg_emit_error("%s", "remote database not attached (misuse)");
		return (NULL);
	}

	sql = sbuf_new_auto();
	sbuf_cat(sql, basesql);

	pkg_config_bool(PKG_CONFIG_MULTIREPOS, &multirepos_enabled);

	if (multirepos_enabled) {
		/*
		 * Working on multiple remote repositories
		 */

		/* add the dbname column to the SELECT */
		sbuf_cat(sql, ", dbname FROM (");

		if (reponame != NULL) {
			if (is_attached(db->sqlite, reponame)) {
				sbuf_printf(sql, multireposql, reponame, reponame);
			} else {
				pkg_emit_error("Repository %s can't be loaded", reponame);
				return (NULL);
			}
		} else {
			/* test on all the attached databases */
			if (sql_on_all_attached_db(db->sqlite, sql, multireposql) != EPKG_OK)
				return (NULL);
		}

		/* close the UNIONs and build the search query */
		sbuf_cat(sql, ") WHERE ");
		pkgdb_rquery_build_search_query(sql, match, field);
		sbuf_finish(sql);
	} else {
		/* 
		 * Working on a single remote repository
		 */

		sbuf_cat(sql, ", 'remote' AS dbname FROM remote.packages WHERE ");
		pkgdb_rquery_build_search_query(sql, match, field);
		sbuf_cat(sql, ";");
		sbuf_finish(sql);
	}

	if (sqlite3_prepare_v2(db->sqlite, sbuf_get(sql), -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (NULL);
	}

	sbuf_delete(sql);

	sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);

	return (pkgdb_it_new(db, stmt, PKG_REMOTE));
}

int
pkgdb_integrity_append(struct pkgdb *db, struct pkg *p)
{
	int ret = EPKG_OK;

	sqlite3_stmt *stmt = NULL;
	sqlite3_stmt *stmt_conflicts = NULL;
	struct pkg_file *file = NULL;
	struct sbuf *conflictmsg = sbuf_new_auto();

	const char sql[] = "INSERT INTO integritycheck (name, origin, version, path)"
		"values (?1, ?2, ?3, ?4);";
	const char sql_conflicts[] = "SELECT name, version from integritycheck where path=?1;";

	assert( db != NULL && p != NULL);

	sql_exec(db->sqlite, "CREATE TEMP TABLE IF NOT EXISTS integritycheck ( "
			"name TEXT, "
			"origin TEXT, "
			"version TEXT, "
			"path TEXT UNIQUE);"
		);

	if (sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}
	while (pkg_files(p, &file) == EPKG_OK) {
		const char *name, *origin, *version;

		pkg_get(p, PKG_NAME, &name, PKG_ORIGIN, &origin, PKG_VERSION, &version);
		sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, origin, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 3, version, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 4, pkg_file_get(file, PKG_FILE_PATH), -1, SQLITE_STATIC);

		if (sqlite3_step(stmt) != SQLITE_DONE) {
			sbuf_clear(conflictmsg);
			sbuf_printf(conflictmsg, "WARNING: %s-%s conflict on %s with: \n",
			    name, version,
			    pkg_file_get(file, PKG_FILE_PATH));

			if (sqlite3_prepare_v2(db->sqlite, sql_conflicts, -1, &stmt_conflicts, NULL) != SQLITE_OK) {
				ERROR_SQLITE(db->sqlite);
				sqlite3_finalize(stmt);
				sbuf_delete(conflictmsg);
				return (EPKG_FATAL);
			}

			sqlite3_bind_text(stmt_conflicts, 1, pkg_file_get(file, PKG_FILE_PATH), -1, SQLITE_STATIC);

			while (sqlite3_step(stmt_conflicts) != SQLITE_DONE) {
				sbuf_printf(conflictmsg, "\t- %s-%s\n",
						sqlite3_column_text(stmt_conflicts, 0),
						sqlite3_column_text(stmt_conflicts, 1));
			}
			sqlite3_finalize(stmt_conflicts);
			sbuf_finish(conflictmsg);
			pkg_emit_error(sbuf_get(conflictmsg));
			ret = EPKG_FATAL;
		}
		sqlite3_reset(stmt);
	}
	sqlite3_finalize(stmt);
	sbuf_delete(conflictmsg);

	return (ret);
}

int
pkgdb_integrity_check(struct pkgdb *db)
{
	int ret = EPKG_OK;
	sqlite3_stmt *stmt;
	sqlite3_stmt *stmt_conflicts;
	struct sbuf *conflictmsg = sbuf_new_auto();
	assert (db != NULL);

	const char sql_local_conflict[] = "SELECT p.name, p.version FROM packages AS p, files AS f "
		"WHERE p.id = f.package_id AND f.path = ?1;";

	const char sql_conflicts[] = "SELECT name, version from integritycheck where path=?1;";

	if (sqlite3_prepare_v2(db->sqlite,
		"SELECT path, COUNT(path) from ("
		"SELECT path from integritycheck UNION ALL "
		"SELECT path from files, main.packages AS p where p.id=package_id and p.origin NOT IN (SELECT origin from integritycheck)"
		") GROUP BY path HAVING (COUNT(path) > 1 );",
		-1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(db->sqlite);
		return (EPKG_FATAL);
	}

	while (sqlite3_step(stmt) != SQLITE_DONE) {
		sbuf_clear(conflictmsg);

		if (sqlite3_prepare_v2(db->sqlite, sql_local_conflict, -1, &stmt_conflicts, NULL) != SQLITE_OK) {
			ERROR_SQLITE(db->sqlite);
			sqlite3_finalize(stmt);
			sbuf_delete(conflictmsg);
			return (EPKG_FATAL);
		}

		sqlite3_bind_text(stmt_conflicts, 1, sqlite3_column_text(stmt, 0), -1, SQLITE_STATIC);

		sqlite3_step(stmt_conflicts);

		sbuf_printf(conflictmsg, "WARNING: locally installed %s-%s conflicts on %s with:\n",
				sqlite3_column_text(stmt_conflicts, 0),
				sqlite3_column_text(stmt_conflicts, 1),
				sqlite3_column_text(stmt, 0)
				);
		
		sqlite3_finalize(stmt_conflicts);

		if (sqlite3_prepare_v2(db->sqlite, sql_conflicts, -1, &stmt_conflicts, NULL) != SQLITE_OK) {
			ERROR_SQLITE(db->sqlite);
			sqlite3_finalize(stmt);
			sbuf_delete(conflictmsg);
			return (EPKG_FATAL);
		}

		sqlite3_bind_text(stmt_conflicts, 1, sqlite3_column_text(stmt, 0), -1, SQLITE_STATIC);

		while (sqlite3_step(stmt_conflicts) != SQLITE_DONE) {
			sbuf_printf(conflictmsg, "\t- %s-%s\n",
					sqlite3_column_text(stmt_conflicts, 0),
					sqlite3_column_text(stmt_conflicts, 1));
		}
		sqlite3_finalize(stmt_conflicts);
		sbuf_finish(conflictmsg);
		pkg_emit_error(sbuf_get(conflictmsg));
		ret = EPKG_FATAL;
	}

	sqlite3_finalize(stmt);
	sbuf_delete(conflictmsg);

/*	sql_exec(db->sqlite, "DROP TABLE IF EXISTS integritycheck");*/

	return (ret);
}

struct pkgdb_it *
pkgdb_integrity_conflict_local(struct pkgdb *db, const char *origin)
{
       sqlite3_stmt *stmt;

       assert(db != NULL && origin != NULL);

       const char sql_conflicts [] = "SELECT DISTINCT p.id as rowid, p.origin, p.name, p.version, p.prefix "
               "FROM packages AS p, files AS f, integritycheck AS i "
               "WHERE p.id = f.package_id AND f.path = i.path AND i.origin = ?1";

       if (sqlite3_prepare_v2(db->sqlite, sql_conflicts, -1, &stmt, NULL) != SQLITE_OK) {
               ERROR_SQLITE(db->sqlite);
               return (NULL);
       }

       sqlite3_bind_text(stmt, 1, origin, -1, SQLITE_TRANSIENT);

       return (pkgdb_it_new(db, stmt, PKG_INSTALLED));
}

