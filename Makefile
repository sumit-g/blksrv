CC = g++
OUT_FILE_NAME = libblksrv.a

CFLAGS= -fPIC -Wall -c -fpermissive

INC = -I./include

OBJ_DIR=./obj

OUT_DIR=./lib

# Enumerating of every *.cc as *.o and using that as dependency
$(OUT_FILE_NAME): $(patsubst src/%.cc,$(OBJ_DIR)/%.o,$(wildcard src/*.cc))
	ar -r -o $(OUT_DIR)/$@ $^

#Compiling every *.cc to *.o
$(OBJ_DIR)/%.o: src/%.cc dirmake
	$(CC) -c $(INC) $(CFLAGS) -o $@  $<
	
dirmake:
	@mkdir -p $(OUT_DIR)
	@mkdir -p $(OBJ_DIR)
	
clean:
	rm -f $(OBJ_DIR)/*.o $(OUT_DIR)/$(OUT_FILE_NAME) Makefile.bak

rebuild: clean build
