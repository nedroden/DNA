AM_CPPFLAGS =						\
	-I$(top_srcdir)					\
	-I$(top_builddir)

nautilus_extensiondir=$(NAUTILUS_EXTENSION_DIR)

nautilus_extension_LTLIBRARIES=libnautilus-dropbox.la

libnautilus_dropbox_la_CFLAGS = 	                \
	-DDATADIR=\"$(datadir)\"					    \
	-DEMBLEMDIR=\"$(EMBLEM_DIR)\"					\
	-Wall                                           \
	$(WARN_CFLAGS)                                  \
	$(DISABLE_DEPRECATED_CFLAGS)					\
	$(NAUTILUS_CFLAGS)                              \
	$(GLIB_CFLAGS)

if DEBUG
libnautilus_dropbox_la_CFLAGS += -DND_DEBUG
else
libnautilus_dropbox_la_CFLAGS += -DG_DISABLE_ASSERT -DG_DISABLE_CHECKS
endif

libnautilus_dropbox_la_SOURCES = \
	nautilus-dropbox.cpp       \
	nautilus-dropbox.h       \
	nautilus-dropbox-hooks.h \
	nautilus-dropbox-hooks.cpp \
	dropbox-command-client.h \
	dropbox-command-client.cpp \
	dropbox-client.c dropbox-client.h \
	g-util.h \
	async-io-coroutine.h \
	dropbox-client-util.cpp \
	dropbox-client-util.h \
	dropbox.cpp \
	dna-util.cpp \
	dna-util.h

libnautilus_dropbox_la_LDFLAGS = -module -avoid-version
libnautilus_dropbox_la_LIBADD  = $(NAUTILUS_LIBS) $(GLIB_LIBS)
