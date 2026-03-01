CC = gcc
CFLAGS = -O3 -Wall -Wextra

CUDA_ROOT_DIR=/usr/local/cuda-13.0
CUDA_ARCH=sm_100

CUDA_LIB_DIR=-L$(CUDA_ROOT_DIR)/lib/cuda-13.0
CUDA_INC_DIR=-I$(CUDA_ROOT_DIR)/include
NVCC = $(CUDA_ROOT_DIR)/bin/nvcc
NVCCFLAGS = -O3 -arch=$(CUDA_ARCH) $(CUDA_INC_DIR) $(CUDA_LIB_DIR) -Xcompiler "-Wall -Wextra"

TARGET = frontend_study
CUDA_TARGET = fe_study_cuda
THROUGHPUT_TARGET = instr_throughput
BTB_TARGET = btb_estimate
BTB_CALLS_TARGET = btb_estimate_calls
BTB_CALLS_2M_TARGET = btb_estimate_calls_2M
OBJS = utils.o

all: $(TARGET) $(CUDA_TARGET) $(THROUGHPUT_TARGET) $(BTB_TARGET) $(BTB_CALLS_TARGET) $(BTB_CALLS_2M_TARGET)

$(TARGET): frontend_study.c $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) frontend_study.c $(OBJS)

$(CUDA_TARGET): fe_study_cuda.cu $(OBJS)
	$(NVCC) $(NVCCFLAGS) -o $(CUDA_TARGET) fe_study_cuda.cu $(OBJS)

$(THROUGHPUT_TARGET): instr_throughput.c $(OBJS)
	$(CC) $(CFLAGS) -o $(THROUGHPUT_TARGET) instr_throughput.c $(OBJS)

$(BTB_TARGET): btb_estimate.c $(OBJS)
	$(CC) $(CFLAGS) -o $(BTB_TARGET) btb_estimate.c $(OBJS)

$(BTB_CALLS_TARGET): btb_estimate_calls.c $(OBJS)
	$(CC) $(CFLAGS) -o $(BTB_CALLS_TARGET) btb_estimate_calls.c $(OBJS)

$(BTB_CALLS_2M_TARGET): btb_estimate_calls_2M.c $(OBJS)
	$(CC) $(CFLAGS) -o $(BTB_CALLS_2M_TARGET) btb_estimate_calls_2M.c $(OBJS)

utils.o: utils.c utils.h
	$(CC) $(CFLAGS) -c utils.c

clean:
	rm -f $(TARGET) $(CUDA_TARGET) $(THROUGHPUT_TARGET) $(BTB_TARGET) $(BTB_CALLS_TARGET) $(BTB_CALLS_2M_TARGET) $(THROUGHPUT_DYNAMIC_TARGET) $(OBJS)

.PHONY: all clean
