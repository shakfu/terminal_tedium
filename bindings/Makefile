# Engine bindings.
#
# Each is built only if its SDK is present, so this works on a development
# machine with some engines installed and on the Pi with others. Override the
# auto-detected paths on the command line, e.g.
#
#   make PD_INCLUDE=/usr/include/pd
#
# Nothing here is required to use libtedium: a plain C or C++ program links
# the library directly and needs no binding at all.

UNAME_S := $(shell uname -s)
TOP     := ..
BUILD   := $(TOP)/build/bindings
LIB     := $(TOP)/build/libtedium.a

# Pd's class_new takes a t_newmethod, and every external in existence casts an
# incompatibly-typed constructor to it. The warning is inherent to the API.
CFLAGS  += -O2 -fPIC -I$(TOP)/include -Wall -Wextra -Wno-unused-parameter \
           -Wno-cast-function-type -Wno-cast-function-type-mismatch
LDLIBS  += -lm

ifeq ($(UNAME_S),Linux)
  LDLIBS += -lpthread
  PD_EXT := pd_linux
  PD_LDFLAGS := -shared
  CH_EXT := chug
  CH_LDFLAGS := -shared
  CS_EXT := so
  CS_LDFLAGS := -shared
else
  PD_EXT := pd_darwin
  PD_LDFLAGS := -bundle -undefined dynamic_lookup
  CH_EXT := chug
  CH_LDFLAGS := -bundle -undefined dynamic_lookup
  CS_EXT := dylib
  CS_LDFLAGS := -bundle -undefined dynamic_lookup
endif

# ------------------------------------------------------------------ #
# SDK discovery                                                       #
# ------------------------------------------------------------------ #

# Pure Data: m_pd.h
PD_INCLUDE ?= $(firstword $(wildcard \
    /usr/include/pd \
    /usr/local/include/pd \
    /opt/homebrew/include/pd \
    $(wildcard /Applications/Pd*.app/Contents/Resources/src) \
    $(wildcard /Applications/*/Pd*.app/Contents/Resources/src)))

# Csound: csdl.h
CS_INCLUDE ?= $(firstword $(wildcard \
    /usr/include/csound \
    /usr/local/include/csound \
    /opt/homebrew/include/csound \
    /Library/Frameworks/CsoundLib64.framework/Headers))

# ChucK: chugin.h / chuck_dl.h. There is no system install location, so
# point CK_INCLUDE at a chuck source tree's src/chuck directory.
CK_INCLUDE ?= $(firstword $(wildcard \
    /usr/local/include/chuck \
    /opt/homebrew/include/chuck \
    $(HOME)/chuck/src/chuck))

# JACK
JACK_CFLAGS := $(shell pkg-config --cflags jack 2>/dev/null)
JACK_LIBS   := $(shell pkg-config --libs jack 2>/dev/null)

TARGETS :=
ifneq ($(PD_INCLUDE),)
  TARGETS += pd
endif
ifneq ($(CS_INCLUDE),)
  TARGETS += csound
endif
ifneq ($(CK_INCLUDE),)
  TARGETS += chuck
endif
ifneq ($(JACK_LIBS),)
  TARGETS += jack
endif

.PHONY: all pd csound chuck jack clean report

all: report $(TARGETS)

report:
	@echo "bindings to build: $(if $(TARGETS),$(TARGETS),none)"
	@echo "  pd      $(if $(PD_INCLUDE),$(PD_INCLUDE),NOT FOUND - set PD_INCLUDE)"
	@echo "  csound  $(if $(CS_INCLUDE),$(CS_INCLUDE),NOT FOUND - set CS_INCLUDE)"
	@echo "  chuck   $(if $(CK_INCLUDE),$(CK_INCLUDE),NOT FOUND - set CK_INCLUDE)"
	@echo "  jack    $(if $(JACK_LIBS),$(JACK_LIBS),NOT FOUND - install jack dev package)"
	@mkdir -p $(BUILD)

# ------------------------------------------------------------------ #

pd: $(BUILD)/tedium~.$(PD_EXT)

$(BUILD)/tedium~.$(PD_EXT): pd/tedium~.c $(LIB)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(PD_INCLUDE) $(PD_LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)

csound: $(BUILD)/libtedium_opcodes.$(CS_EXT)

$(BUILD)/libtedium_opcodes.$(CS_EXT): csound/tedium_opcodes.c $(LIB)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(CS_INCLUDE) $(CS_LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)

chuck: $(BUILD)/Tedium.$(CH_EXT)

$(BUILD)/Tedium.$(CH_EXT): chuck/tedium_chugin.cpp $(LIB)
	@mkdir -p $(BUILD)
	$(CXX) $(CFLAGS) -std=c++11 -I$(CK_INCLUDE) $(CH_LDFLAGS) -o $@ $< \
	    $(LIB) $(LDLIBS)

jack: $(BUILD)/tedium-jack

$(BUILD)/tedium-jack: jack/tedium-jack.c $(LIB)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(JACK_CFLAGS) -o $@ $< $(LIB) $(JACK_LIBS) $(LDLIBS)

clean:
	rm -rf $(BUILD)
