# makefile to fail if any command in pipe is failed.
SHELL = /usr/bin/env bash
.SHELLFLAGS = -o pipefail -c

MAKEFLAGS += "-j $(shell nproc)"

# using gcc version 10.2.1
BASE    = arm-none-linux-gnueabihf

CC      = $(BASE)-gcc
LD      = $(BASE)-ld
STRIP   = $(BASE)-strip

ifeq ($(V),1)
	Q :=
else
	Q := @
endif

INCLUDE	= -I./
INCLUDE	+= -I./lib/libco
INCLUDE	+= -I./lib/miniz
INCLUDE	+= -I./lib/md5
INCLUDE += -I./lib/lzma
INCLUDE += -I./lib/zstd/lib
INCLUDE += -I./lib/libchdr/include
INCLUDE += -I./lib/bluetooth
INCLUDE += -I./lib/serial_server/library
INCLUDE += -I./third_party/sqlite
INCLUDE += -I./$(BUILDDIR)

BUILDDIR = bin

PRJ = MiSTer
C_SRC =   $(wildcard *.c) \
          $(wildcard ./third_party/sqlite/*.c) \
          $(wildcard ./lib/miniz/*.c) \
          $(wildcard ./lib/md5/*.c) \
          $(wildcard ./lib/lzma/*.c) \
						$(wildcard ./lib/zstd/lib/common/*.c) \
						$(wildcard ./lib/zstd/lib/decompress/*.c) \
          $(wildcard ./lib/libchdr/*.c) \
          lib/libco/arm.c

CPP_SRC = $(wildcard *.cpp) \
          $(wildcard ./lib/serial_server/library/*.cpp) \
          $(wildcard ./support/*/*.cpp)

IMG =     $(wildcard *.png)
SQLITE_SRAM_MIGRATIONS = $(wildcard ./support/sqlite_sram/migrations/*.sql)
SQLITE_SRAM_MIGRATIONS_HDR = $(BUILDDIR)/sqlite_sram_migrations_autogen.h
SQLITE_SRAM_MIGRATIONS_OBJ = $(BUILDDIR)/./support/sqlite_sram/migrations.cpp.o
SQLITE_SRAM_MIGRATIONS_DEP = $(BUILDDIR)/./support/sqlite_sram/migrations.cpp.d

IMLIB2_LIB  = -Llib/imlib2 -lfreetype -lbz2 -lpng16 -lz -lImlib2

OBJ	= $(C_SRC:%.c=$(BUILDDIR)/%.c.o) $(CPP_SRC:%.cpp=$(BUILDDIR)/%.cpp.o) $(IMG:%.png=$(BUILDDIR)/%.png.o)
DEP	= $(C_SRC:%.c=$(BUILDDIR)/%.c.d) $(CPP_SRC:%.cpp=$(BUILDDIR)/%.cpp.d)

DFLAGS	= $(INCLUDE) -D_7ZIP_ST -DPACKAGE_VERSION=\"1.3.3\" -DHAVE_LROUND -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_SYS_PARAM_H -DENABLE_64_BIT_WORDS=0 -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -DVDATE=\"`date +"%y%m%d"`\" -DSQLITE_SRAM_SNAPSHOTS=1 -DSQLITE_OMIT_LOAD_EXTENSION
CFLAGS	= $(DFLAGS) -Wall -Wextra -Wno-strict-aliasing -Wno-stringop-overflow -Wno-stringop-truncation -Wno-format-truncation -Wno-psabi -Wno-restrict -c
LFLAGS	= -lc -lstdc++ -lm -lrt $(IMLIB2_LIB) -Llib/bluetooth -lbluetooth -lpthread

OUTPUT_FILTER = sed -e 's/\(.[a-zA-Z]\+\):\([0-9]\+\):\([0-9]\+\):/\1(\2,\ \3):/g'

ifneq ($(DEBUG),1)
	CFLAGS += -O3
else
	CFLAGS += -O0 -g -fomit-frame-pointer
endif

ifeq ($(PROFILING),1)
	DFLAGS += -DPROFILING
endif

$(BUILDDIR)/$(PRJ): $(OBJ)
	$(Q)$(info $@)
	$(Q)$(CC) -o $@ $+ $(LFLAGS)
	$(Q)cp $@ $@.elf
ifneq ($(DEBUG),1)
	$(Q)$(STRIP) $@
endif

.PHONY: clean
clean:
	$(Q)rm -rf bin

$(BUILDDIR)/%.c.o: %.c
	$(Q)$(info $<)
	$(Q)$(CC) $(CFLAGS) -std=gnu99 -o $@ -c $< 2>&1 | $(OUTPUT_FILTER)

$(BUILDDIR)/%.cpp.o: %.cpp
	$(Q)$(info $<)
	$(Q)$(CC) $(CFLAGS) -std=gnu++14 -Wno-class-memaccess -o $@ -c $< 2>&1 | $(OUTPUT_FILTER)

$(BUILDDIR)/%.png.o: %.png
	$(Q)$(info $<)
	$(Q)$(LD) -r -b binary -o $@ $< 2>&1 | $(OUTPUT_FILTER)

$(SQLITE_SRAM_MIGRATIONS_HDR): support/sqlite_sram/gen_migrations_header.sh $(SQLITE_SRAM_MIGRATIONS)
	@mkdir -p $(dir $@)
	$(Q)./support/sqlite_sram/gen_migrations_header.sh $@

$(SQLITE_SRAM_MIGRATIONS_OBJ): $(SQLITE_SRAM_MIGRATIONS_HDR)
$(SQLITE_SRAM_MIGRATIONS_DEP): $(SQLITE_SRAM_MIGRATIONS_HDR)

ifneq ($(MAKECMDGOALS), clean)
-include $(DEP)
endif
$(BUILDDIR)/%.c.d: %.c
	@mkdir -p $(dir $(BUILDDIR)/$*)
	$(Q)$(info $< >> $@)
	$(Q)$(CC) $(DFLAGS) -MM $< -MT $@ -MT $*.c.o -MF $@ 2>&1 | $(OUTPUT_FILTER)

$(BUILDDIR)/%.cpp.d: %.cpp
	@mkdir -p $(dir $(BUILDDIR)/$*)
	$(Q)$(info $< >> $@)
	$(Q)$(CC) $(DFLAGS) -MM $< -MT $@ -MT $*.cpp.o -MF $@ 2>&1 | $(OUTPUT_FILTER)

# Ensure correct time stamp
$(BUILDDIR)/main.cpp.o: $(filter-out $(BUILDDIR)/main.cpp.o, $(OBJ))
