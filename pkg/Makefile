PROG?=		pkg
SRCS=		add.c \
		audit.c \
		autoremove.c \
		backup.c \
		check.c \
		clean.c \
		create.c \
		delete.c \
		event.c \
		info.c \
		install.c \
		main.c \
		progressmeter.c \
		query.c \
		register.c \
		repo.c \
		update.c \
		upgrade.c \
		search.c \
		updating.c \
		utils.c \
		version.c \
		which.c
PREFIX?=	/usr/local
BINDIR=		${PREFIX}/sbin
MANDIR=		${PREFIX}/man/man

.if exists(../.git) && exists(/usr/local/bin/git)
GITHASH!=	git rev-parse HEAD
CFLAGS+=	-DGITHASH=\"${GITHASH}\"
.endif

DEBUG_FLAGS+=	-g -O0
.if defined(PROFILE_BUILD)
DEBUG_FLAGS+=	-pg
.endif

CFLAGS+=	-I${.CURDIR}/../libpkg
LDADD+=		-L${.OBJDIR}/../libpkg \
		-lpkg \
		-lutil \
		-ljail \
		${LDADD_STATIC}

WARNS?=		6
MAN=	pkg.1 \
	pkg-add.1 \
	pkg-autoremove.1 \
	pkg-backup.1 \
	pkg-create.1 \
	pkg-delete.1 \
	pkg-info.1 \
	pkg-install.1 \
	pkg-query.1 \
	pkg-register.1 \
	pkg-repo.1 \
	pkg-search.1 \
	pkg-update.1 \
	pkg-upgrade.1 \
	pkg-version.1 \
	pkg-which.1 \
	pkg.conf.5

.include <bsd.prog.mk>
