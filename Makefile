CC = clang
CFLAGS = -O2 -Wall -Wextra
FRAMEWORKS = -framework IOKit -framework CoreFoundation -framework SystemConfiguration
SRCS = main.c temp.c cpu.c ram.c storage.c

mi_temp: $(SRCS) report.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(FRAMEWORKS)

clean:
	rm -f mi_temp
