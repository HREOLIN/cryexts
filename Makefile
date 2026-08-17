KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m := cryexts.o
cryexts-y := super.o crypto.o metadata.o balloc.o inode.o dir.o file.o journal.o xattr.o

ccflags-y += -Wall

.PHONY: all module tools clean test-image

all: module tools

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

tools: mkfs.cryexts cryextsck cryexts_journal_inject cryexts_journal_v2_inject cryexts_journal_v3_inject cryexts_orphan_inject cryexts_extent_inspect cryexts_dir_index_inspect cryexts_policy_inspect cryexts_journal_inspect cryexts_alloc_inspect cryexts_xattr_inspect cryexts_gdt_inspect

mkfs.cryexts: tools/mkfs.cryexts.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/mkfs.cryexts.c

cryextsck: tools/cryextsck.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryextsck.c

cryexts_journal_inject: tools/cryexts_journal_inject.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryexts_journal_inject.c

cryexts_journal_v2_inject: tools/cryexts_journal_v2_inject.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryexts_journal_v2_inject.c

cryexts_journal_v3_inject: tools/cryexts_journal_v3_inject.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryexts_journal_v3_inject.c

cryexts_orphan_inject: tools/cryexts_orphan_inject.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryexts_orphan_inject.c

cryexts_extent_inspect: tools/cryexts_extent_inspect.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryexts_extent_inspect.c

cryexts_dir_index_inspect: tools/cryexts_dir_index_inspect.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryexts_dir_index_inspect.c

cryexts_policy_inspect: tools/cryexts_policy_inspect.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryexts_policy_inspect.c

cryexts_journal_inspect: tools/cryexts_journal_inspect.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryexts_journal_inspect.c

cryexts_alloc_inspect: tools/cryexts_alloc_inspect.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryexts_alloc_inspect.c

cryexts_xattr_inspect: tools/cryexts_xattr_inspect.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryexts_xattr_inspect.c

cryexts_gdt_inspect: tools/cryexts_gdt_inspect.c cryexts_fs.h
	$(CC) -Wall -Wextra -O2 -I. -o $@ tools/cryexts_gdt_inspect.c

test-image: mkfs.cryexts
	dd if=/dev/zero of=cryexts.img bs=1M count=64
	./mkfs.cryexts -f cryexts.img

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) mkfs.cryexts cryextsck cryexts_journal_inject cryexts_journal_v2_inject cryexts_journal_v3_inject cryexts_orphan_inject cryexts_extent_inspect cryexts_dir_index_inspect cryexts_policy_inspect cryexts_journal_inspect cryexts_alloc_inspect cryexts_xattr_inspect cryexts_gdt_inspect cryexts.img
