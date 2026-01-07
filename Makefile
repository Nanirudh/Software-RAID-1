KDIR ?= $(shell realpath $(PWD)/../../../../..)

obj-m += ssr.o
ssr-y := ssr_main.o ssr_io.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
