# ====================================================================
# GENERATED FILE -- do not edit.
#
#   source:      <ace tool>/manifests/loaders/*.loader.json
#   regenerate:  ace manifest generate loaders
#
# One Makefile serves every loader, selected by FILE=. A loader with
# its own <Name>.loader.json gets an ifeq block below; every other
# loader builds from the defaults alone.
# ====================================================================

UNAME_S := $(shell uname -s)
NPROC   := $(shell nproc)
CXX := g++
BIN_DIR := ../bin

# Debug info: make DEBUG=1, off by default. A sanitizer build forces
# it on -- see the module Makefiles' own note.
DEBUGFLAGS :=
ifeq ($(DEBUG),1)
    DEBUGFLAGS := -g
endif

# Sanitizers: make ASAN=1 / make TSAN=1, mutually exclusive.
#
# A loader and the modules it dlopens share one address space, so they
# must be built the SAME way -- `ace make module X ASAN=1` for every
# module the loader will load, not just the loader. A sanitizer runtime
# that sees only half the process reports nonsense.
SANITIZE :=
ifeq ($(ASAN),1)
ifeq ($(TSAN),1)
    $(error ASAN=1 and TSAN=1 are mutually exclusive)
endif
    SANITIZE := -fsanitize=address -fno-omit-frame-pointer
endif
ifeq ($(TSAN),1)
    SANITIZE := -fsanitize=thread -fno-omit-frame-pointer
endif
ifneq ($(SANITIZE),)
    DEBUGFLAGS := -g
endif

CXXFLAGS := -std=c++17 \
            -fvisibility=hidden \
            -fpermissive \
            -Wall \
            -Wextra \
            -O2 \
            -I../.. \
            -pipe \
            -fno-plt \
            -DETCS_LOADER \
            -DETCS_MODULE_NAME=\"ROOT\" \
            $(DEBUGFLAGS) \
            $(SANITIZE)

ifeq ($(UNAME_S),Linux)
    LDFLAGS := -ldl -Wl,--no-as-needed -pthread -luring -Wl,--as-needed -fuse-ld=gold -Wl,--threads -Wl,--thread-count,$(NPROC)
else ifeq ($(UNAME_S),Windows_NT)
    LDFLAGS := -lws2_32
else
    $(error Unsupported platform: $(UNAME_S))
endif

# Per-loader additions. EXTRA_LINK is kept OUT of LDFLAGS on purpose:
# it lands after the translation unit on the command line, because
# gold does not rescan an archive it has already passed.
EXTRA_CXXFLAGS :=
EXTRA_LINK :=

ifeq ($(FILE),SCSRealTesterLoader)
    # inherits: NetworkProvider
    EXTRA_CXXFLAGS += -DMBEDTLS_THREADING_C -DMBEDTLS_THREADING_PTHREAD -I../modules/NetworkProvider/picohttpparser -I../modules/NetworkProvider/mbedtls/include -I../modules/NetworkProvider/mbedtls/tf-psa-crypto/include -I../modules/NetworkProvider/mbedtls/tf-psa-crypto/drivers/builtin/include -I../modules/NetworkProvider/mbedtls/tf-psa-crypto/drivers/everest/include -I../modules/NetworkProvider/mbedtls/tf-psa-crypto/drivers/pqcp/include -I../modules/NetworkProvider/mbedtls/tf-psa-crypto/drivers/p256-m
    EXTRA_LINK += ../modules/NetworkProvider/mbedtls/build/library/libmbedtls.a ../modules/NetworkProvider/mbedtls/build/library/libmbedx509.a ../modules/NetworkProvider/mbedtls/build/library/libmbedcrypto.a -ldl
endif

# If FILE is specified, build only that one; otherwise every *Loader.cc.
ifdef FILE
    LOADER_SRCS := $(FILE).cc
else
    LOADER_SRCS := $(wildcard *Loader.cc)
endif
LOADER_BINS := $(LOADER_SRCS:.cc=)

.PHONY: all clean copy_loaders
all: $(LOADER_BINS) etcs copy_loaders

%Loader: %Loader.cc
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) $(EXTRADEFINES) -o Run_$@ $< $(EXTRA_LINK) $(LDFLAGS)

etcs: etcs.cc
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) $(EXTRADEFINES) -o $@ $< $(EXTRA_LINK) $(LDFLAGS)

copy_loaders:
	@mkdir -p $(BIN_DIR)
	@echo "--- Moving Loaders to $(BIN_DIR)/ ---"
	@found=0; \
	for f in Run_*Loader etcs; do \
	    if [ -f "$$f" ]; then \
	        mv -f "$$f" $(BIN_DIR)/; \
	        echo " [✓] Moved: $$f -> $(BIN_DIR)/"; \
	        found=1; \
	    fi; \
	done; \
	if [ $$found -eq 0 ]; then echo " [!] No binaries found to move"; fi

clean:
	rm -f Run_*Loader etcs *.o
	@for f in Run_*Loader etcs; do rm -f $(BIN_DIR)/$$f; done
