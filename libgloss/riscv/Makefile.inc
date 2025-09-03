
CPPFLAGS += -march=rv32ima_zicsr -I./%D%

define sed-y
	's:^[[:space:]]*\.ascii[[:space:]]*"\(.*\)".*:\1:; \
	/^->/{s:->#\(.*\):/* \1 */:; \
	s:^->\([^ ]*\) [\$$#]*\([-0-9]*\) \(.*\):#define \1 \2 /* \3 */:; \
	s:^->\([^ ]*\) [\$$#]*\([^ ]*\) \(.*\):#define \1 \2 /* \3 */:; \
	s:->::; p;}'
endef

define make_asm_offsets
	(set -e; \
	 echo "#ifndef __LIBGLOSS_RISCV_ASM_OFFSETS_H"; \
	 echo "#define __LIBGLOSS_RISCV_ASM_OFFSETS_H"; \
	 echo ""; \
	 echo "/*"; \
	 echo " * Generated file. DO NOT MODIFY."; \
	 echo " */"; \
	 echo ""; \
	 sed -ne $(sed-y) $<; \
	 echo ""; \
	 echo "#endif /* __LIBGLOSS_RISCV_ASM_OFFSETS_H */" ) > $@
endef

%D%/asm-offsets.s: %D%/asm-offsets.c
	$(AM_V_CC)$(COMPILE) -S -o $@ $<

%D%/asm-offsets.h: %D%/asm-offsets.s
	$(call make_asm_offsets)

%D%/crt0.o: %D%/asm-offsets.h

CLEANFILES += %D%/asm-offsets.s %D%/asm-offsets.h

multilibtool_DATA += \
	%D%/nano.specs \
	%D%/crt0.o

multilibtool_LIBRARIES += %D%/libgloss.a
%C%_libgloss_a_CPPFLAGS = -I$(srcdir)/%D%
%C%_libgloss_a_SOURCES = \
	%D%/sys_access.c \
	%D%/sys_chdir.c \
	%D%/sys_chmod.c \
	%D%/sys_chown.c \
	%D%/sys_close.c \
	%D%/sys_conv_stat.c \
	%D%/sys_execve.c \
	%D%/sys_exit.c \
	%D%/sys_faccessat.c \
	%D%/sys_fork.c \
	%D%/sys_fstat.c \
	%D%/sys_fstatat.c \
	%D%/sys_ftime.c \
	%D%/sys_getcwd.c \
	%D%/sys_getpid.c \
	%D%/sys_gettimeofday.c \
	%D%/sys_isatty.c \
	%D%/sys_kill.c \
	%D%/sys_link.c \
	%D%/sys_lseek.c \
	%D%/sys_lstat.c \
	%D%/sys_open.c \
	%D%/sys_openat.c \
	%D%/sys_read.c \
	%D%/sys_sbrk.c \
	%D%/sys_stat.c \
	%D%/sys_sysconf.c \
	%D%/sys_times.c \
	%D%/sys_unlink.c \
	%D%/sys_utime.c \
	%D%/sys_wait.c \
	%D%/sys_write.c \
	%D%/mlock.c \
	%D%/lock.c \
	%D%/_os.c

includetool_DATA += %D%/_os.h

includemachinetooldir = $(tooldir)/include/machine
includemachinetool_DATA = %D%/machine/syscall.h
