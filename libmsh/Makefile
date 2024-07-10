CC=gcc

BUILD=build
SRC=.

TARGET=$(BUILD)/libmsh.so

INCLUDES=-I$(SRC)/
CFLAGS=-Wall -I$(INCLUDES) -std=c11 -fno-omit-frame-pointer -mno-red-zone
HEADERS=$(wildcard $(SRC)/*.h)

all: prebuild $(TARGET)

prebuild:
	@mkdir -p $(BUILD)

$(TARGET): $(BUILD)/msh.o $(BUILD)/pthread.o
	$(CC) $(INCLUDES) $(CFLAGS) -shared -fPIC -o $@ $^ -ldl -mfsgsbase

$(BUILD)/msh.o: $(SRC)/msh.c $(HEADERS)
	$(CC) $(INCLUDES) $(CFLAGS) -shared -fPIC -c $< -o $@ -mfsgsbase

$(BUILD)/pthread.o: $(SRC)/pthread.c $(HEADERS)
	$(CC) $(INCLUDES) $(CFLAGS) -shared -fPIC -c $< -o $@ -mfsgsbase

.PHONY: clean
clean:
	rm -rf $(BUILD)
