CFLAGS=-O3 -g -Wall -Wno-unused-function -Wno-misleading-indentation -Wno-unused-variable
rboemu: cpu.c hle.c win.c dx.c d3d.c dshow.c sched.c pe.c main.c cpu.h hle.h dxshared.h
	gcc $(CFLAGS) -o rboemu cpu.c hle.c win.c dx.c d3d.c dshow.c sched.c pe.c main.c -lm

LIBSRC=cpu.c hle.c win.c dx.c d3d.c dshow.c sched.c pe.c main.c rbo_api.c
librbo.a: $(LIBSRC) cpu.h hle.h dxshared.h rbo_api.h
	mkdir -p build_lib && for f in $(LIBSRC); do gcc $(CFLAGS) -DRBO_LIBRARY -c $$f -o build_lib/$$f.o || exit 1; done
	ar rcs librbo.a build_lib/*.o
test_api: librbo.a test_api.c
	gcc $(CFLAGS) -o test_api test_api.c librbo.a -lm -lpthread
