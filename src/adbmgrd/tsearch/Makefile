#-------------------------------------------------------------------------
#
# Makefile for mgr/tsearch
#
# Copyright (c) 2006-2016, PostgreSQL Global Development Group
#
# src/adbmgrd/tsearch/Makefile
#
#-------------------------------------------------------------------------
subdir = src/adbmgrd/tsearch
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = ts_locale.o ts_parse.o wparser.o wparser_def.o dict.o \
	dict_simple.o dict_synonym.o dict_thesaurus.o \
	dict_ispell.o regis.o spell.o \
	to_tsany.o ts_selfuncs.o ts_typanalyze.o ts_utils.o

include $(top_srcdir)/src/adbmgrd/common.mk

.PHONY: install-data
.PHONY: uninstall-data
install-data uninstall-data:
	$(MAKE) -C $(backend_obj_dir)/$(cur_dir) $@
