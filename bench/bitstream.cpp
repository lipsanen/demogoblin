#include "benchmark/benchmark.h"
extern "C" {
#include "bitstream.h"
}
#include <cstring>
#include <vector>

const std::size_t SIZE = 1 << 15;

static void bitstream_bench(benchmark::State &state) {
  const std::size_t SIZE = 1 << 10;
  char* memory = (char*)std::malloc(SIZE);
  std::memset(memory, 0xee, SIZE);

  srand(0);
  std::vector<unsigned> reads;
  bitstream stream = bitstream_create(memory, SIZE * 8);
  std::size_t size_left = SIZE * 8;

  while(size_left > 0) {
    std::size_t bits = rand() % 15;
    unsigned bits_iteration = std::min(size_left, bits);
    reads.push_back(bits_iteration);
    size_left -= bits_iteration;
  }

  for (auto _ : state) {
    stream.bitoffset = 0;
    for(size_t i=0; i < reads.size(); ++i) {
      bitstream_read_uint(&stream, reads[i]);
    }
  }

  state.SetBytesProcessed(SIZE * state.iterations());
  std::free(memory);
}

static char* prepare_array() {
  char* memory = (char*)std::malloc(SIZE + 16);
  std::memset(memory, 0,SIZE);

  srand(0);

  for(size_t i=0; i < SIZE; ++i) {
    memory[i] = rand() % 32;
  }

  return memory;
}

static void bitstream_bench_ubitvar(benchmark::State &state) {
  char* memory = prepare_array();

  for (auto _ : state) {
    bitstream stream = bitstream_create(memory, SIZE * 8);

    while(stream.bitoffset < stream.bitsize && !stream.overflow)
      bitstream_read_ubitvar(&stream);
    
    benchmark::DoNotOptimize(stream);
  }

  state.SetBytesProcessed(SIZE * state.iterations());
  std::free(memory);
}

static void bitstream_bench_field_index(benchmark::State &state) {
  char* memory = prepare_array();
  bitstream stream = bitstream_create(memory, SIZE * 8);

  for (auto _ : state) {
    stream.bitoffset = 0;
    stream.buffered_bytes_read = 0;
    stream.overflow = false;

    while(stream.bitoffset < stream.bitsize && !stream.overflow)
      bitstream_read_field_index(&stream, -1, true);
  }

  state.SetBytesProcessed(SIZE * state.iterations());
  std::free(memory);
}

BENCHMARK(bitstream_bench);
BENCHMARK(bitstream_bench_ubitvar);
BENCHMARK(bitstream_bench_field_index);
