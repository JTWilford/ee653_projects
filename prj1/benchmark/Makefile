GCC=g++
CPP_FILES = $(shell ls *.cpp)
OBJ_FILES = $(CPP_FILES:.cpp=.o)
CPP_COMPILE_FILES = -g -Wall -std=c++11
RM = rm -rf
JUNK = $(OBJ_FILES) predict
CLASS_NUM = 10
VEC_PER_CLASS = 10
INPUT_SIZE = 128
CPP_COMPILE_FILES += -DCLASS_NUM=$(CLASS_NUM) -DVEC_PER_CLASS=$(VEC_PER_CLASS) -DINPUT_SIZE=$(INPUT_SIZE)

all: predict

predict: $(OBJ_FILES)
	@$(GCC) $(OBJ_FILES) -o $@

%.o: %.cpp
	@$(GCC) -c $< -o $@ $(CPP_COMPILE_FILES)

clean:
	@$(RM) $(JUNK)
