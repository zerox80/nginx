NGINX_DIR := ../..
TARGET := $(NGINX_DIR)/objs/http_v3_request_body_test

CC ?= cc
CFLAGS ?= -O0 -g
CPPFLAGS += -I$(NGINX_DIR)/objs -I$(NGINX_DIR)/src/core \
	-I$(NGINX_DIR)/src/event -I$(NGINX_DIR)/src/event/quic \
	-I$(NGINX_DIR)/src/os/unix -I$(NGINX_DIR)/src/http \
	-I$(NGINX_DIR)/src/http/modules -I$(NGINX_DIR)/src/http/v3

SOURCES := http_v3_request_body_test.c \
	$(NGINX_DIR)/src/http/v3/ngx_http_v3_parse.c \
	$(NGINX_DIR)/src/http/v3/ngx_http_v3.c \
	$(NGINX_DIR)/src/core/ngx_buf.c \
	$(NGINX_DIR)/src/core/ngx_palloc.c \
	$(NGINX_DIR)/src/os/unix/ngx_alloc.c

HEADERS := $(wildcard $(NGINX_DIR)/src/core/*.h) \
	$(wildcard $(NGINX_DIR)/src/event/*.h) \
	$(wildcard $(NGINX_DIR)/src/event/quic/*.h) \
	$(wildcard $(NGINX_DIR)/src/os/unix/*.h) \
	$(wildcard $(NGINX_DIR)/src/http/*.h) \
	$(wildcard $(NGINX_DIR)/src/http/v3/*.h)

.PHONY: all check clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) Makefile \
	$(NGINX_DIR)/src/http/v3/ngx_http_v3_request.c \
	$(NGINX_DIR)/objs/ngx_auto_config.h \
	$(NGINX_DIR)/objs/ngx_auto_headers.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror \
		-Wno-unused-parameter -ffunction-sections -fdata-sections \
		$(SOURCES) $(LDFLAGS) -Wl,--gc-sections $(LDLIBS) -o $@

check: $(TARGET)
	$(TARGET)

clean:
	rm -f $(TARGET)
