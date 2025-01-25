# ─────────────────────────────────────────────────────────────────────
# Makefile
# Builds:
#   1) watchman_test  (the test program for hardware watchpoints)
#   2) watchman       (the external attach-based watchpoint debugger)
# ─────────────────────────────────────────────────────────────────────

CC      = gcc
CFLAGS  = -Wall -g -O0 -fno-omit-frame-pointer -rdynamic
LIBS    = -lpthread -lbacktrace

# Executables
TARGET1 = watchman_test
TARGET2 = watchman

# Object files for watchman_test
OBJS1   = watchman_test.o

# Object file for watchman
OBJS2   = watchman.o

# Default rule: build both
all: $(TARGET1) $(TARGET2)

# ─────────────────────────────────────────────────────────────────────
# Rules for building watchman_test
$(TARGET1): $(OBJS1)
	$(CC) $(CFLAGS) -o $@ $(OBJS1) $(LIBS)

watchman_test.o: watchman_test.c
	$(CC) $(CFLAGS) -c watchman_test.c

# ─────────────────────────────────────────────────────────────────────
# Rules for building watchman
$(TARGET2): $(OBJS2)
	$(CC) $(CFLAGS) -o $@ $(OBJS2) $(LIBS)

watchman.o: watchman.c
	$(CC) $(CFLAGS) -c watchman.c

# ─────────────────────────────────────────────────────────────────────
# Cleanup
clean:
	rm -f *.o $(TARGET1) $(TARGET2)
