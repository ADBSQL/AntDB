#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/lmgr
#
# IDENTIFICATION
#    src/adbmgrd/storage/lmgr/Makefile
#
#-------------------------------------------------------------------------

subdir = src/adbmgrd/storage/lmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = lmgr.o lock.o proc.o deadlock.o lwlock.o lwlocknames.o spin.o \
	s_lock.o predicate.o condition_variable.o

include $(top_srcdir)/src/adbmgrd/common.mk

ifdef TAS
TASPATH = $(top_builddir)/src/adbmgrd/port/tas.o
endif

s_lock_test: s_lock.c $(top_builddir)/src/port/libpgport.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -DS_LOCK_TEST=1 $(srcdir)/s_lock.c \
		$(TASPATH) -L $(top_builddir)/src/port -lpgport -o s_lock_test

# see explanation in ../../parser/Makefile
lwlocknames.c: lwlocknames.h ;

lwlocknames.h: $(top_srcdir)/src/adbmgrd/storage/lmgr/lwlocknames.txt $(backend_src_dir)/$(cur_dir)/generate-lwlocknames.pl
	$(PERL) $(backend_src_dir)/$(cur_dir)/generate-lwlocknames.pl $<

check: s_lock_test
	./s_lock_test

clean distclean:
	rm -f s_lock_test

maintainer-clean: clean
	rm -f lwlocknames.h lwlocknames.c
