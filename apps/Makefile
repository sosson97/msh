CCP=g++

PRM_SRC=src/primary
SCAV_SRC=src/scavengers
BUILD=build
CMPC_LIST=../profile/results/cmpc_list.txt
CMPC_LIST=../profile/results/pred_prof.txt
CMPC_LIST=../profile/results/lat_prof.txt
CPPFLAGS=-Wall -I$(INCLUDES) -std=c++17 -fno-omit-frame-pointer -mno-red-zone -fpermissive

all: pre-build primary scav

pre-build:
	@mkdir -p $(BUILD)
	@mkdir -p $(BUILD)/asms

primary: $(BUILD)/ptrchase $(BUILD)/template

scav: $(BUILD)/compute.so

$(BUILD)/ptrchase: $(PRM_SRC)/ptrchase.cpp
	$(CCP) $(CPPFLAGS) -o $@ $< -lpthread

$(BUILD)/template: $(PRM_SRC)/template.cpp
	$(CCP) $(CPPFLAGS) -o $@ $< -lpthread

$(BUILD)/%.so: $(SCAV_SRC)/%.cpp
	$(CCP) $(CPPFLAGS) -shared -fPIC -o $@ $^


$(BUILD)/%.so.bolt: $(BUILD)/%.so
	llvm-bolt $< -o $@ \
		--clh-cmpc-list=$(CMPC_LIST) \
		--disable-ctx-prefetch \
		--disable-pseudo-inline	\
		--disable-asympp \
		--assume-abi \
		--clh-opt-yield \
		--test-special-yield \
		--inst-scav \
		--lat-prof=$(LAT_PROF) \
		--pred-prof=$(PRED_PROF) \
		--bound-yield-distance=$(YIELD_DISTANCE) \
		--clh-prob-th=200 > out.txt 2> err.txt #/dev/null

$(BUILD)/%.bolt: $(BUILD)/%
	llvm-bolt $< -o $@ \
		--clh-cmpc-list=$(CMPC_LIST) \
		--disable-ctx-prefetch \
		--assume-abi \
		--clh-opt-yield \
		--clh-prob-th=75 > out.txt 2> err.txt #/dev/null

$(BUILD)/asms/%.asm: $(BUILD)/%
	objdump -s -S $< > $@
