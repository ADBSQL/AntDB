backend_src_dir = $(abs_top_srcdir)/src/backend
backend_obj_dir = $(top_builddir)/src/backend
mgr_inc_dir = $(top_builddir)/src/adbmgrd/include

override CFLAGS := $(patsubst -DADB,, $(CFLAGS))
override CFLAGS += -DADBMGRD -I$(mgr_inc_dir) -I$(top_srcdir)/src/adbmgrd/include -I$(top_srcdir)/$(subdir) -I$(top_srcdir)/src/interfaces

cur_dir = $(subdir:src/adbmgrd/%=%)

override VPATH = $(abs_top_srcdir)/$(subdir):$(backend_src_dir)/$(cur_dir)
override with_llvm = no
include $(backend_src_dir)/common.mk

