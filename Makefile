CC      = gcc
CFLAGS  = -m32 -O2 -std=gnu11 -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-builtin \
          -fno-strict-aliasing -fno-tree-loop-distribute-patterns -mno-sse -mno-mmx -mno-sse2 -mfpmath=387 \
          -Wall -Wno-unused-function -Wno-misleading-indentation -Isrc
LIBGCC  = $(shell $(CC) -m32 -print-libgcc-file-name)
SRC     = boot.S lib.c arch.c gfx.c svg.c fs.c ui.c apps.c apps2.c taskmgr.c net.c netapp.c fontdata.c \
          kmalloc.c clip.c png.c zip.c miniz_kt.c miniz_malloc.c \
          vendor/kcompat.c \
          browser.c \
          discord.c \
          vendor/bearssl/bearssl.c \
          vendor/quickjs/quickjs_kt.c
OBJ     = $(addprefix build/,$(addsuffix .o,$(SRC)))

all: ktuyome.iso

build/%.c.o: src/%.c src/ktuyome.h
	@mkdir -p build/vendor/bearssl build/vendor/quickjs
	$(CC) $(CFLAGS) -c $< -o $@
build/%.S.o: src/%.S
	@mkdir -p build
	$(CC) -m32 -c $< -o $@
build/kernel.elf: $(OBJ) src/linker.ld
	ld -m elf_i386 -nostdlib -T src/linker.ld -o $@ $(OBJ) $(LIBGCC)
ktuyome.iso: build/kernel.elf
	@mkdir -p build/iso/boot/grub
	cp build/kernel.elf build/iso/boot/ktuyome.elf
	printf 'set timeout=0\nset gfxpayload=keep\nmenuentry "Ktuyome" {\n  multiboot /boot/ktuyome.elf\n  boot\n}\n' > build/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ build/iso 2>/dev/null
run: ktuyome.iso
	@[ -f disk.img ] || qemu-img create -f raw disk.img 64M
	qemu-system-i386 -m 256 -cdrom ktuyome.iso -hda disk.img -boot d -vga std \
	  -netdev user,id=net0 \
	  -device e1000,netdev=net0
runnet: ktuyome.iso
	@[ -f disk.img ] || qemu-img create -f raw disk.img 64M
	qemu-system-i386 -m 256 -cdrom ktuyome.iso -hda disk.img -boot d -vga std \
	  -netdev user,id=net0,hostfwd=tcp::8080-:80 \
	  -device e1000,netdev=net0
clean:
	rm -rf build ktuyome.iso
