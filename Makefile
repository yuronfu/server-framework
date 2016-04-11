EXEC = \
	test-async \
	test-reactor \
	test-buffer \
	test-protocol-server \
	httpd

OUT ?= .build
.PHONY: all
all: $(OUT) $(EXEC)

CC ?= gcc
CFLAGS = -std=gnu99 -Wall -O2 -g -I .
LDFLAGS = -lpthread

ifeq ($(strip $(PROFILE)),1)
PROF_FLAGS = -pg
CFLAGS += $(PROF_FLAGS) 
endif

OBJS := \
	async.o \
	async-lockfree.o \
	reactor.o \
	buffer.o \
	protocol-server.o

ifeq ($(strip $(LOCKFREE)),1)
OBJS := $(filter-out async.o,$(OBJS))
CFLAGS += -DLOCKFREE
else
OBJS := $(filter-out async-lockfree.o,$(OBJS))
endif

deps := $(OBJS:%.o=%.o.d)
OBJS := $(addprefix $(OUT)/,$(OBJS))
deps := $(addprefix $(OUT)/,$(deps))

httpd: $(OBJS) httpd.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test-%: $(OBJS) tests/test-%.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUT)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ -MMD -MF $@.d $<

$(OUT):
	@mkdir -p $@

doc:
	@doxygen

clean:
	$(RM) $(EXEC) $(OBJS) $(deps)
	@rm -rf $(OUT)

distclean: clean
	rm -rf html

-include $(deps)
